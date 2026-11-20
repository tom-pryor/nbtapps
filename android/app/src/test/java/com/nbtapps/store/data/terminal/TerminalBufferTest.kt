// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.terminal

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class TerminalBufferTest {

    private fun bytes(vararg values: Int) = ByteArray(values.size) { values[it].toByte() }

    private fun text(s: String) = s.encodeToByteArray()

    private fun linesOf(buffer: TerminalBuffer) = buffer.snapshot().lines.map { it.text }

    /** What [input] makes, written whole. */
    private fun whole(input: ByteArray, buffer: TerminalBuffer = TerminalBuffer()): List<String> {
        buffer.write(input)
        return linesOf(buffer)
    }

    /**
     * Asserts that [input] makes [expected] written whole, cut in two at every byte, and a byte at a time: how a
     * shell's output comes is never what it means.
     */
    private fun assertLines(what: String, input: ByteArray, expected: List<String>, make: () -> TerminalBuffer = { TerminalBuffer() }) {
        assertEquals("$what, whole", expected, whole(input, make()))
        for (cut in 1 until input.size) {
            val buffer = make()
            buffer.write(input, 0, cut)
            buffer.write(input, cut, input.size - cut)
            assertEquals("$what, cut at $cut", expected, linesOf(buffer))
        }
        val buffer = make()
        for (i in input.indices) buffer.write(input, i, 1)
        assertEquals("$what, a byte at a time", expected, linesOf(buffer))
    }

    @Test
    fun `the control characters, each as a table of what goes in and what comes out`() {
        val table = listOf(
            Triple("plain text", text("abc"), listOf("abc")),
            Triple("a line feed", text("abc\ndef"), listOf("abc", "def")),
            Triple("a line feed at the end leaves an empty line, not shown", text("abc\n"), listOf("abc")),
            Triple("CR LF pairs", text("one\r\ntwo\r\nthree"), listOf("one", "two", "three")),
            Triple("LF CR", text("one\n\rtwo"), listOf("one", "two")),
            Triple("empty lines between", text("a\n\n\nb"), listOf("a", "", "", "b")),
            Triple("a line feed alone", text("\n"), listOf("")),
            Triple("a carriage return writes over", text("hello\rHE"), listOf("HEllo")),
            Triple("a prompt written over after a carriage return", text("$ \r# ls\r\nfile\r\n# "), listOf("# ls", "file", "# ")),
            Triple("a progress line", text("10%\r20%\r100%\n"), listOf("100%")),
            Triple("a carriage return then a longer line", text("ab\rwxyz"), listOf("wxyz")),
            Triple("backspaces move left and erase nothing", text("abc\b\bX"), listOf("aXc")),
            Triple("the shell's back, space, back", text("abc\b \bd"), listOf("abd")),
            Triple("a backspace at column 0", text("\b\bx"), listOf("x")),
            Triple("a backspace does not go up a line", text("a\n\bb"), listOf("a", "b")),
            Triple("a tab to column 8", text("a\tb"), listOf("a       b")),
            Triple("a tab from column 0", text("\tx"), listOf("        x")),
            Triple("a tab from a tab stop", text("12345678\tx"), listOf("12345678        x")),
            Triple("a tab over text moves the cursor only", text("abcdefghij\r\tX"), listOf("abcdefghXj")),
            Triple("a tab that ends past the text pads it", text("abc\r\tX"), listOf("abc     X")),
            Triple("the bell is dropped", text("a\u0007b"), listOf("ab")),
            Triple("DEL is dropped", text("a\u007fb"), listOf("ab")),
            Triple("a C1 control is dropped", text("a\u0085b\u009bc"), listOf("abc")),
            Triple("vertical tab and form feed are dropped", text("a\u000bb\u000cc"), listOf("abc")),
        )
        for ((what, input, expected) in table) assertLines(what, input, expected)
    }

    @Test
    fun `every other C0 control character is dropped`() {
        val acted = setOf(0x08, 0x09, 0x0A, 0x0D, 0x1B)
        for (c in 0x00 until 0x20) {
            if (c in acted) continue
            assertEquals("0x${c.toString(16)}", listOf("ab"), whole(bytes('a'.code, c, 'b'.code)))
        }
    }

    @Test
    fun `escape sequences are dropped whole, cut at every byte`() {
        val table = listOf(
            "a colour" to "a\u001b[31mb\u001b[0m",
            "a CSI with parameters and a private mark" to "a\u001b[?25l\u001b[1;31;42mb",
            "clear screen and home" to "\u001b[2J\u001b[Hab",
            "a CSI with an intermediate" to "a\u001b[2 qb",
            "an OSC ended by BEL" to "a\u001b]0;a title\u0007b",
            "an OSC ended by ESC backslash" to "a\u001b]0;a title\u001b\\b",
            "an OSC with UTF-8 in it" to "a\u001b]2;tïtle €\u0007b",
            "a hyperlink" to "a\u001b]8;;https://example.invalid/\u001b\\b\u001b]8;;\u001b\\",
            "a DCS" to "a\u001bPq#0;2;0;0;0\u001b\\b",
            "an APC, a PM and an SOS" to "a\u001b_apc\u001b\\\u001b^pm\u001b\\\u001bXsos\u001b\\b",
            "save and restore the cursor" to "a\u001b7\u001b8b",
            "keypad modes" to "a\u001b=\u001b>b",
            "a charset designation" to "a\u001b(B\u001b)0b",
            "a single shift" to "a\u001bOb",
            "ESC ESC begins anew" to "a\u001b\u001b[1mb",
            "an ESC in a CSI begins anew" to "a\u001b[1\u001b[2mb",
            "CAN ends a CSI" to "a\u001b[12\u0018b",
            "SUB ends an OSC" to "a\u001b]0;x\u001ab",
            "DEL in a CSI is ignored" to "a\u001b[1\u007fmb",
            "an OSC ended by ESC and another sequence" to "a\u001b]0;x\u001b[mb",
        )
        for ((what, input) in table) assertLines(what, text(input), listOf("ab"))
    }

    @Test
    fun `a control character in a sequence is acted on, and a character that cannot be in one ends it`() {
        assertLines("a carriage return in a CSI", text("ab\u001b[1\rmX"), listOf("Xb"))
        assertLines("a line feed in a CSI", text("a\u001b[1\nmb"), listOf("a", "b"))
        assertLines("a character past ASCII ends a CSI and is shown", text("a\u001b[1éb"), listOf("aéb"))
        // An OSC's control characters other than its ends are its own.
        assertLines("a carriage return in an OSC", text("ab\u001b]0;x\ry\u0007c"), listOf("abc"))
    }

    @Test
    fun `an escape sequence that never ends is given up, and what follows it is shown`() {
        // The ] and 99 characters reach the limit of 100: the rest is shown.
        val input = text("a\u001b]" + "y".repeat(99) + "zzz")
        assertLines("an OSC with no end", input, listOf("azzz")) { TerminalBuffer(maxEscape = 100) }
        assertLines("a CSI with no end", text("a\u001b[" + "1".repeat(99)), listOf("a")) { TerminalBuffer(maxEscape = 100) }
        assertLines("a CSI past the limit", text("a\u001b[" + "1".repeat(99) + "23"), listOf("a23")) { TerminalBuffer(maxEscape = 100) }

        val buffer = TerminalBuffer()
        buffer.write(text("before\n\u001b]"))
        buffer.write(text("x".repeat(TerminalBuffer.MAX_ESCAPE - 1)))
        buffer.write(text("shown"))
        assertEquals(listOf("before", "shown"), linesOf(buffer))
    }

    @Test
    fun `multi-byte characters, cut at every byte`() {
        assertLines("two, three and four bytes", text("é€😀 ok"), listOf("é€😀 ok"))
        assertLines("a carriage return counts characters, not bytes", text("😀é\rX"), listOf("Xé"))
        assertLines("a backspace over a four-byte character", text("a😀\bb"), listOf("ab"))
        // U+0080 is a C1 control, and dropped.
        assertLines("the edges of each length", text("\u0080߿ࠀ￿𐀀􏿿"), listOf("߿ࠀ￿𐀀􏿿"))
    }

    @Test
    fun `bytes that are not UTF-8 become U+FFFD`() {
        val r = "�"
        val table = listOf(
            Triple("a byte that is never UTF-8", bytes(0x61, 0xFF, 0x62), "a${r}b"),
            Triple("F5 and above", bytes(0xF5, 0xF8, 0xFC), r.repeat(3)),
            Triple("a lone continuation byte", bytes(0x61, 0x80, 0x62), "a${r}b"),
            Triple("an overlong two-byte form", bytes(0xC0, 0xAF), r.repeat(2)),
            Triple("an overlong three-byte form", bytes(0xE0, 0x80, 0xAF), r.repeat(3)),
            Triple("a surrogate", bytes(0xED, 0xA0, 0x80), r.repeat(3)),
            Triple("past U+10FFFF", bytes(0xF4, 0x90, 0x80, 0x80), r.repeat(4)),
            Triple("a character cut short by ASCII", bytes(0xE2, 0x82, 0x78), "${r}x"),
            Triple("a character cut short by another", bytes(0xE2, 0x82, 0xC3, 0xA9), "${r}é"),
            Triple("a character cut short by ESC, which begins a sequence", bytes(0xE2, 0x1B, 0x5B, 0x6D, 0x61), "${r}a"),
        )
        for ((what, input, expected) in table) assertLines(what, input, listOf(expected))
    }

    @Test
    fun `an unfinished character waits for the next piece, and the end of the output shows what is left as U+FFFD`() {
        val buffer = TerminalBuffer()
        buffer.write(bytes(0x61, 0xE2, 0x82))
        assertEquals(listOf("a"), linesOf(buffer))
        buffer.write(bytes(0xAC))
        assertEquals(listOf("a€"), linesOf(buffer))

        buffer.write(bytes(0xF0, 0x9F))
        buffer.finish()
        assertEquals(listOf("a€�"), linesOf(buffer))
        // And an unfinished sequence is dropped at the end: what comes next is a new shell's.
        buffer.write(text("\u001b]0;tit"))
        buffer.finish()
        buffer.write(text("le\u0007b"))
        assertEquals(listOf("a€�leb"), linesOf(buffer))
    }

    @Test
    fun `at most so many lines, the oldest dropped first`() {
        assertLines("three lines", text("1\n2\n3\n4\n5"), listOf("3", "4", "5")) { TerminalBuffer(maxLines = 3) }
        assertLines("three, the last empty", text("1\n2\n3\n4\n"), listOf("3", "4")) { TerminalBuffer(maxLines = 3) }

        val buffer = TerminalBuffer()
        buffer.write(text((0 until 6_000).joinToString("\n")))
        val lines = linesOf(buffer)
        assertEquals(TerminalBuffer.MAX_LINES, lines.size)
        assertEquals("1000", lines.first())
        assertEquals("5999", lines.last())
    }

    @Test
    fun `a line is cut at so many characters and goes on in a new line`() {
        assertLines("cut at four", text("abcdefghij"), listOf("abcd", "efgh", "ij")) { TerminalBuffer(maxColumns = 4) }
        assertLines("a carriage return after a cut is on the new line", text("abcdef\rX"), listOf("abcd", "Xf")) { TerminalBuffer(maxColumns = 4) }
        assertLines("a tab at the limit", text("abcdefghijklmnop\tx"), listOf("abcdefghijklmnop", "        x")) { TerminalBuffer(maxColumns = 16) }
        assertLines("writing over does not cut", text("abcd\rwxyz"), listOf("wxyz")) { TerminalBuffer(maxColumns = 4) }

        val buffer = TerminalBuffer()
        buffer.write(ByteArray(10_000) { 'x'.code.toByte() })
        assertEquals(listOf(4_096, 4_096, 1_808), linesOf(buffer).map { it.length })
    }

    @Test
    fun `binary output stays within the limits`() {
        val buffer = TerminalBuffer()
        val random = java.util.Random(42)
        repeat(200) {
            val piece = ByteArray(4_096).also(random::nextBytes)
            buffer.write(piece)
        }
        val lines = buffer.snapshot().lines
        assertTrue(lines.size <= TerminalBuffer.MAX_LINES)
        assertTrue(lines.all { it.text.codePointCount(0, it.text.length) <= TerminalBuffer.MAX_COLUMNS })
    }

    @Test
    fun `a mark is a line of its own, flagged, and the shell goes on after it`() {
        val buffer = TerminalBuffer()
        buffer.write(text("one\r\n$ "))
        buffer.mark("The shell ended")
        buffer.write(text("two"))
        assertEquals(
            listOf(TerminalLine("one"), TerminalLine("$ "), TerminalLine("The shell ended", mark = true), TerminalLine("two")),
            buffer.snapshot().lines,
        )

        // On an empty line it takes that line's place.
        val empty = TerminalBuffer()
        empty.write(text("one\n"))
        empty.mark("New shell")
        empty.mark("a\nb")
        assertEquals(
            listOf(TerminalLine("one"), TerminalLine("New shell", true), TerminalLine("a", true), TerminalLine("b", true)),
            empty.snapshot().lines,
        )

        // After a carriage return, in the middle of a line, the whole line stays.
        val middle = TerminalBuffer()
        middle.write(text("abcdef\r"))
        middle.mark("m")
        middle.write(text("x"))
        assertEquals(listOf("abcdef", "m", "x"), linesOf(middle))

        // A mark is cut as a line of the shell's is, and counts against the lines kept.
        val small = TerminalBuffer(maxLines = 3, maxColumns = 4)
        small.mark("abcdefgh😀")
        assertEquals(listOf("efgh", "😀"), linesOf(small))
    }

    @Test
    fun `a mark in the middle of an escape sequence leaves the sequence to be dropped whole`() {
        val buffer = TerminalBuffer()
        buffer.write(text("a\u001b[3"))
        buffer.mark("m")
        buffer.write(text("1mb"))
        assertEquals(listOf("a", "m", "b"), linesOf(buffer))
    }

    @Test
    fun `clear empties the finished lines and keeps the one being written, and a sequence cut across it is still dropped`() {
        val buffer = TerminalBuffer()
        buffer.write(text("one\ntwo\n$ \u001b[3"))
        val before = buffer.snapshot()
        buffer.clear()
        val after = buffer.snapshot()
        assertEquals(listOf(TerminalLine("$ ")), after.lines)
        assertNotEquals(before.revision, after.revision)
        // What Copy all gives is what is shown.
        assertEquals("$ ", buffer.text())
        // The shell's echo goes on in the prompt's line, at the cursor.
        buffer.write(text("1mls\r\nfile\r\n$ "))
        assertEquals(listOf("$ ls", "file", "$ "), linesOf(buffer))
    }

    @Test
    fun `clear keeps a character half read and the cursor where it was`() {
        val buffer = TerminalBuffer()
        val euro = "€".encodeToByteArray()
        buffer.write(text("abc\rx"))
        buffer.write(euro, 0, 1)
        buffer.clear()
        assertEquals(listOf("xbc"), linesOf(buffer))
        buffer.write(euro, 1, euro.size - 1)
        assertEquals(listOf("x€c"), linesOf(buffer))
    }

    @Test
    fun `clear with no line being written leaves nothing`() {
        val buffer = TerminalBuffer()
        buffer.write(text("one\ntwo\n"))
        buffer.mark("The shell ended")
        buffer.clear()
        assertEquals(emptyList<TerminalLine>(), buffer.snapshot().lines)
        assertEquals("", buffer.text())
    }

    @Test
    fun `reset empties it whole, the line being written, a character half read and a sequence begun`() {
        val buffer = TerminalBuffer()
        buffer.write(text("one\n$ \u001b[3"))
        val before = buffer.snapshot()
        buffer.reset()
        assertEquals(emptyList<TerminalLine>(), buffer.snapshot().lines)
        assertNotEquals(before.revision, buffer.snapshot().revision)
        // What follows is not taken as the rest of the sequence.
        buffer.write(text("1mx"))
        assertEquals(listOf("1mx"), linesOf(buffer))

        buffer.reset()
        buffer.write(bytes(0xE2, 0x82))
        buffer.reset()
        buffer.write(text("ok"))
        assertEquals(listOf("ok"), linesOf(buffer))
        assertEquals("ok", buffer.text())
    }

    @Test
    fun `a snapshot is the same one while nothing changes, and stays as it was when something does`() {
        val buffer = TerminalBuffer()
        buffer.write(text("one\ntwo"))
        val first = buffer.snapshot()
        assertSame(first, buffer.snapshot())
        // An escape sequence, a carriage return and a backspace change nothing to be seen.
        buffer.write(text("\u001b[1m\r\b"))
        assertSame(first, buffer.snapshot())

        buffer.write(text("\nthree"))
        val second = buffer.snapshot()
        assertNotEquals(first.revision, second.revision)
        assertEquals(listOf("one", "two"), first.lines.map { it.text })
        assertEquals(listOf("one", "two", "three"), second.lines.map { it.text })
        assertEquals("one\ntwo\nthree", second.text)

        buffer.mark("m")
        assertNotEquals(second.revision, buffer.snapshot().revision)
        assertEquals("one\ntwo\nthree\nm", buffer.text())
    }
}

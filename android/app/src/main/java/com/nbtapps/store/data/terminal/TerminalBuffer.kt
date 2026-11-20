// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.terminal

/** One line of a [TerminalBuffer]: the shell's, or with [mark] one of the app's own ([TerminalBuffer.mark]). */
data class TerminalLine(val text: String, val mark: Boolean = false)

/**
 * The lines of a [TerminalBuffer] at one moment, oldest first, and its [revision], another number after every change.
 * The buffer gives the same snapshot again for as long as nothing changes, and a snapshot is equal only to itself, so
 * a flow of them compares in no time, whatever their length.
 */
class TerminalSnapshot(val lines: List<TerminalLine>, val revision: Long) {
    /** The whole text, a line to a line, the app's own lines too: what Copy copies. */
    val text: String get() = lines.joinToString("\n") { it.text }

    override fun toString(): String = "TerminalSnapshot(revision=$revision, lines=${lines.size})"
}

/**
 * What a shell has written, as lines of text, for a terminal that shows no escape sequences: bytes go in as they come
 * from the shell ([write]), in pieces cut anywhere, and lines come out ([snapshot]).
 *
 * It is a list of lines and a cursor's column on the last of them, the one being written:
 *  - a carriage return takes the cursor to column 0, so what follows writes over what is there; a line feed begins a
 *    new line, at column 0; a backspace moves the cursor one to the left and erases nothing (a shell erases with
 *    "back, space, back"); a tab moves it to the next multiple of 8, with spaces where the line is shorter. The bell and
 *    every other control character are dropped, C1's (U+0080 to U+009F) among them.
 *  - Escape sequences are dropped whole and none is acted on: `ESC [` to its final byte, `ESC ]` to BEL or `ESC \`,
 *    `ESC P`, `ESC X`, `ESC ^` and `ESC _` likewise, and the escapes of two characters, or three with an intermediate.
 *    A control character within one is acted on, as a terminal does; CAN and SUB end one, and ESC begins another. How
 *    far a sequence has come is kept between pieces, so one cut anywhere is dropped all the same; one that runs past
 *    [maxEscape] characters is given up, and what follows it is shown.
 *  - UTF-8 is decoded across pieces: an unfinished character's bytes wait for the next piece. Bytes that are not UTF-8
 *    become U+FFFD, one for each of the longest runs that began a character and could not end it, as WHATWG decodes.
 *  - At most [maxLines] lines are kept, the one being written among them, the oldest dropped first. A line is cut at
 *    [maxColumns] characters and goes on in a new line, so output with no line feeds (binary) cannot grow one line
 *    without end. Nothing is wrapped: the buffer does not know the screen's width.
 *
 * A column is a code point: the buffer knows nothing of how wide a character is drawn.
 *
 * Its calls may come from any thread.
 */
class TerminalBuffer(
    private val maxLines: Int = MAX_LINES,
    private val maxColumns: Int = MAX_COLUMNS,
    private val maxEscape: Int = MAX_ESCAPE,
) {
    init {
        require(maxLines >= 2 && maxColumns >= 1 && maxEscape >= 2)
    }

    /** The lines before the one being written. */
    private val done = ArrayDeque<TerminalLine>()

    /** The line being written, as code points: [length] of them. */
    private var line = IntArray(INITIAL_COLUMNS)
    private var length = 0
    private var column = 0

    private var revision = 0L
    private var given: TerminalSnapshot? = null

    // The UTF-8 decoder, WHATWG's: how many bytes the character still needs, and those it has.
    private var needed = 0
    private var seen = 0
    private var codePoint = 0
    private var lower = 0x80
    private var upper = 0xBF

    // The escape sequence that has begun, if one has, and how many characters it has had.
    private var escape = Escape.NONE
    private var escapeLength = 0

    private enum class Escape { NONE, START, INTERMEDIATE, CSI, STRING, STRING_ESC }

    /** Takes [count] bytes of the shell's output from [bytes] at [offset], as they came. */
    @Synchronized
    fun write(bytes: ByteArray, offset: Int = 0, count: Int = bytes.size - offset) {
        var at = offset
        val end = offset + count
        while (at < end) {
            if (decode(bytes[at].toInt() and 0xFF)) at++
        }
    }

    /**
     * The shell's output has ended: an unfinished character is shown as U+FFFD, and an unfinished escape sequence is
     * dropped, so that nothing of it is taken into what the next shell writes.
     */
    @Synchronized
    fun finish() {
        if (needed > 0) {
            resetDecoder()
            character(REPLACEMENT)
        }
        escape = Escape.NONE
    }

    /**
     * Adds [text], a line of the app's own, flagged as such ([TerminalLine.mark]), on a line of its own: what the shell
     * had begun on the line being written stays a line of its own before it, and the shell goes on on a new line after
     * it. A line feed in [text] begins another such line.
     */
    @Synchronized
    fun mark(text: String) {
        if (length > 0) newLine()
        column = 0
        for (part in text.split('\n')) {
            var at = 0
            do {
                val cut = if (part.codePointCount(at, part.length) > maxColumns) part.offsetByCodePoints(at, maxColumns) else part.length
                done.addLast(TerminalLine(part.substring(at, cut), mark = true))
                at = cut
            } while (at < part.length)
        }
        trim()
        changed()
    }

    /** The lines as they are now: the one being written among them unless it is empty. */
    @Synchronized
    fun snapshot(): TerminalSnapshot {
        given?.takeIf { it.revision == revision }?.let { return it }
        val lines = ArrayList<TerminalLine>(done.size + 1)
        lines.addAll(done)
        if (length > 0) lines.add(TerminalLine(String(line, 0, length)))
        return TerminalSnapshot(lines, revision).also { given = it }
    }

    /** The whole text: [TerminalSnapshot.text]. */
    fun text(): String = snapshot().text

    /**
     * Empties it. A character or an escape sequence that the shell has begun is still taken whole from the next piece,
     * as the shell goes on.
     */
    @Synchronized
    fun clear() {
        done.clear()
        length = 0
        column = 0
        changed()
    }

    /** Decodes [byte]; false where it has to be decoded again, as the first of a new character. */
    private fun decode(byte: Int): Boolean {
        if (needed == 0) {
            when (byte) {
                in 0x00..0x7F -> character(byte)
                in 0xC2..0xDF -> startCharacter(1, byte and 0x1F)
                in 0xE0..0xEF -> {
                    if (byte == 0xE0) lower = 0xA0
                    if (byte == 0xED) upper = 0x9F
                    startCharacter(2, byte and 0x0F)
                }
                in 0xF0..0xF4 -> {
                    if (byte == 0xF0) lower = 0x90
                    if (byte == 0xF4) upper = 0x8F
                    startCharacter(3, byte and 0x07)
                }
                else -> character(REPLACEMENT)
            }
            return true
        }
        if (byte !in lower..upper) {
            // The character cannot end: what it had is one U+FFFD, and the byte begins anew.
            resetDecoder()
            character(REPLACEMENT)
            return false
        }
        lower = 0x80
        upper = 0xBF
        codePoint = (codePoint shl 6) or (byte and 0x3F)
        if (++seen == needed) {
            val decoded = codePoint
            resetDecoder()
            character(decoded)
        }
        return true
    }

    private fun startCharacter(more: Int, bits: Int) {
        needed = more
        seen = 0
        codePoint = bits
    }

    private fun resetDecoder() {
        needed = 0
        seen = 0
        codePoint = 0
        lower = 0x80
        upper = 0xBF
    }

    /** One decoded character, [c]: shown, acted on, or taken into an escape sequence. */
    private fun character(c: Int) {
        if (escape != Escape.NONE && ++escapeLength > maxEscape) {
            // A sequence that does not end: given up, so that what follows is shown.
            escape = Escape.NONE
        }
        when (escape) {
            Escape.NONE -> when {
                c == ESC -> startEscape()
                c < 0x20 || c in 0x7F..0x9F -> control(c)
                else -> put(c)
            }
            Escape.START -> when {
                c == '['.code -> escape = Escape.CSI
                c == ']'.code || c == 'P'.code || c == 'X'.code || c == '^'.code || c == '_'.code -> escape = Escape.STRING
                c in 0x20..0x2F -> escape = Escape.INTERMEDIATE
                c in 0x30..0x7E -> escape = Escape.NONE
                else -> inSequence(c)
            }
            Escape.INTERMEDIATE -> when (c) {
                in 0x20..0x2F -> Unit
                in 0x30..0x7E -> escape = Escape.NONE
                else -> inSequence(c)
            }
            Escape.CSI -> when (c) {
                in 0x20..0x3F -> Unit
                in 0x40..0x7E -> escape = Escape.NONE
                else -> inSequence(c)
            }
            Escape.STRING -> when (c) {
                BEL, CAN, SUB -> escape = Escape.NONE
                ESC -> escape = Escape.STRING_ESC
                // The string's own: dropped with it.
                else -> Unit
            }
            Escape.STRING_ESC -> if (c == '\\'.code) {
                escape = Escape.NONE
            } else {
                // The ESC ended the string, and begins a sequence of its own.
                startEscape()
                character(c)
            }
        }
    }

    /** Begins an escape sequence: an ESC has come. */
    private fun startEscape() {
        escape = Escape.START
        escapeLength = 0
    }

    /** A character in an escape sequence that is no part of it: as a terminal takes it. */
    private fun inSequence(c: Int) {
        when {
            c == ESC -> startEscape()
            c == CAN || c == SUB -> escape = Escape.NONE
            c < 0x20 -> control(c)
            // DEL is ignored within a sequence.
            c == 0x7F -> Unit
            else -> {
                // Not a sequence after all: given up, and the character shown.
                escape = Escape.NONE
                character(c)
            }
        }
    }

    private fun control(c: Int) {
        when (c) {
            CR -> column = 0
            LF -> newLine()
            BS -> if (column > 0) column--
            TAB -> tab()
            // The bell and the rest.
            else -> Unit
        }
    }

    /** [c] at the cursor, over what is there, and the cursor on. */
    private fun put(c: Int) {
        if (column >= maxColumns) newLine()
        if (column == length) {
            if (length == line.size) line = line.copyOf(minOf(line.size * 2, maxColumns))
            length++
        }
        line[column++] = c
        changed()
    }

    private fun tab() {
        if (column >= maxColumns) newLine()
        val stop = minOf((column / TAB_STOP + 1) * TAB_STOP, maxColumns)
        while (column < stop) {
            if (column < length) column++ else put(' '.code)
        }
    }

    private fun newLine() {
        done.addLast(TerminalLine(String(line, 0, length)))
        length = 0
        column = 0
        trim()
        changed()
    }

    /** Drops the oldest lines past [maxLines], the one being written counted. */
    private fun trim() {
        while (done.size > maxLines - 1) done.removeFirst()
    }

    private fun changed() {
        revision++
    }

    companion object {
        /** The lines kept at most. */
        const val MAX_LINES = 5_000

        /** The characters of a line at most: the rest goes on in a new line. */
        const val MAX_COLUMNS = 4_096

        /**
         * The characters of an escape sequence at most, its ESC not counted. Those the app's terminal meets are
         * much shorter; one that runs on is likely a stray ESC in binary output, and is given up.
         */
        const val MAX_ESCAPE = 4_096

        const val TAB_STOP = 8
        const val REPLACEMENT = 0xFFFD

        private const val INITIAL_COLUMNS = 128
        private const val BEL = 0x07
        private const val BS = 0x08
        private const val TAB = 0x09
        private const val LF = 0x0A
        private const val CR = 0x0D
        private const val CAN = 0x18
        private const val SUB = 0x1A
        private const val ESC = 0x1B
    }
}

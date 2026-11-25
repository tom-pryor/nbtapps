// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.about

/**
 * A licence's text as its page shows it: flowed ([flowLicenceText]), and with a paragraph in Markdown's bold markers
 * shown without them ([withoutBoldMarkers]). The text in the app's assets stays as it came with the code.
 */
internal fun shownLicenceText(text: String): String = withoutBoldMarkers(flowLicenceText(text))

/** A line of a text to flow, and whether it begins a line of its own whatever its length. */
private data class TextLine(val text: String, val startsLine: Boolean = false)

/** A list item's first line: "1. ", "(a) ", "b) " and the like, after the line's indent. */
private val LIST_ITEM = Regex("""^\s*(\d+\.|\(\w{1,4}\)|\w\))\s""")

/** A line of a comment frame's inside: " * text", or " *" alone for a blank line. */
private val FRAME_LINE = Regex("""^\s*\*( |$)""")

/**
 * A text wrapped by hand at a fixed width, flowed so that it wraps at the screen's: within a paragraph, a line break
 * that is only there because the line was full becomes a space. A break is kept where it means something:
 *
 * - a blank line, which stays as it is;
 * - a line that, with the next line's first word, would have taken no more than five sixths of the text's widest line:
 *   it ended short of the width on purpose, as "All rights reserved." under a copyright line does;
 * - before and after a line with no letter or digit, such as a rule of dashes;
 * - after a line that begins with "Copyright", which stays a line of its own;
 * - before a list item ("1. ", "(a) "), whose own lines join it with their indent dropped.
 *
 * A text whose paragraphs are each one line, as the Apache License's, comes out as it was. A block framed as a C
 * comment, from a line of a slash and a star to a line of a star and a slash with " * " before each line between, as
 * jBCrypt's licence is, is shown without the frame ([withoutCommentFrames]) and flowed as the rest. Lines lose the
 * spaces at their ends; no word is added, dropped or changed, and the frame's markers are the only marks that go.
 */
internal fun flowLicenceText(text: String): String {
    val lines = withoutCommentFrames(text.lines())
    val width = lines.maxOfOrNull { it.text.trimEnd().length } ?: 0
    val out = mutableListOf<String>()
    var previous: String? = null
    for (line in lines) {
        val current = line.text.trimEnd()
        val before = previous
        val joins = before != null && before.isNotBlank() && current.isNotBlank() && !line.startsLine &&
            !before.isRule() && !current.isRule() && !before.trimStart().startsWith("Copyright") &&
            !LIST_ITEM.containsMatchIn(current) &&
            (before.length + 1 + current.trim().substringBefore(' ').length) * 6 > width * 5
        if (joins) out[out.lastIndex] = out.last() + " " + current.trim() else out += current
        previous = current
    }
    return out.joinToString("\n")
}

/** A line with no letter or digit, such as a rule of dashes. Not a blank one. */
private fun String.isRule() = isNotBlank() && none { it.isLetterOrDigit() }

/**
 * [lines] with each comment frame taken off: the frame's first line (a slash and a star) and its last (a star and a
 * slash) go, each line between loses its " * ", and " *" alone becomes a blank line. The first line inside, and the
 * line after the frame, begin lines of their own. An opening line with no whole frame after it stays as it is.
 */
private fun withoutCommentFrames(lines: List<String>): List<TextLine> {
    val out = mutableListOf<TextLine>()
    var i = 0
    var afterFrame = false
    while (i < lines.size) {
        val end = if (lines[i].trim() == "/*") frameEnd(lines, i) else -1
        if (end < 0) {
            out += TextLine(lines[i], startsLine = afterFrame)
            afterFrame = false
            i++
            continue
        }
        lines.subList(i + 1, end).forEachIndexed { n, inside ->
            out += TextLine(inside.replaceFirst(FRAME_LINE, ""), startsLine = n == 0)
        }
        afterFrame = true
        i = end + 1
    }
    return out
}

/** The index of the line that closes the frame begun at [start], with every line between a frame's; -1 where there is none. */
private fun frameEnd(lines: List<String>, start: Int): Int {
    for (i in start + 1 until lines.size) {
        if (lines[i].trim() == "*/") return i
        if (!FRAME_LINE.containsMatchIn(lines[i])) return -1
    }
    return -1
}

/**
 * [text] with Markdown's bold markers taken off a paragraph that begins and ends with them: "**THE SOFTWARE ...
 * SOFTWARE.**" shows as "THE SOFTWARE ... SOFTWARE.". Nothing else of Markdown is read.
 */
internal fun withoutBoldMarkers(text: String): String {
    val lines = text.lines().toMutableList()
    var start = 0
    while (start < lines.size) {
        if (lines[start].isBlank()) {
            start++
            continue
        }
        var end = start
        while (end + 1 < lines.size && lines[end + 1].isNotBlank()) end++
        val paragraph = lines.subList(start, end + 1).joinToString("\n").trim()
        if (paragraph.length >= 4 && paragraph.startsWith("**") && paragraph.endsWith("**")) {
            lines[start] = lines[start].replaceFirst("**", "")
            lines[end] = lines[end].trimEnd().removeSuffix("**")
        }
        start = end + 1
    }
    return lines.joinToString("\n")
}

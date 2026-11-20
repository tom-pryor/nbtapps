// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.terminal

/**
 * The lines the user sent from the terminal's field, newest last, for Up and Down to bring back into it: the last
 * [limit], kept in memory only, never written anywhere, and gone with the app's process.
 *
 * Up and Down walk them from the field as the user left it. Up from there keeps what the field had (the draft), and
 * Down past the newest line brings it back. A line brought back and then edited is not changed in the history: the
 * edited text becomes the draft, and the walk begins again from the newest line. Sending a line ends the walk.
 *
 * An empty line is not kept, nor a line that is the same as the one before it. Its calls are made on the main thread.
 */
class CommandHistory(private val limit: Int = LIMIT) {
    private val lines = ArrayDeque<String>()

    /** Where the walk is: an index into [lines], or their count for the draft. */
    private var at = 0

    /** What the field had when the walk left it. */
    private var draft = ""

    /** The lines kept, oldest first. */
    val all: List<String> get() = lines.toList()

    /** [line] was sent: it is kept, and the walk begins again from the newest line. */
    fun sent(line: String) {
        if (line.isNotEmpty() && lines.lastOrNull() != line) {
            lines.addLast(line)
            while (lines.size > limit) lines.removeFirst()
        }
        at = lines.size
        draft = ""
    }

    /** The user typed in the field, which now has [text]: that is the draft, and the walk begins again. */
    fun edited(text: String) {
        at = lines.size
        draft = text
    }

    /** The line before the one in the field, which has [current]; null where there is none. */
    fun earlier(current: String): String? {
        if (at <= 0) return null
        if (at >= lines.size) draft = current
        at--
        return lines[at]
    }

    /** The line after the one in the field, or the draft after the newest; null where the field has the draft already. */
    fun later(): String? {
        if (at >= lines.size) return null
        at++
        return if (at == lines.size) draft else lines[at]
    }

    companion object {
        /** How many lines are kept. */
        const val LIMIT = 50
    }
}

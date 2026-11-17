// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.settings

import com.nbtapps.store.data.link.AgentClient
import com.nbtapps.store.data.packages.ManifestParser
import com.nbtapps.store.model.SettingEntry
import com.nbtapps.store.model.SettingFault
import com.nbtapps.store.model.SettingOption
import com.nbtapps.store.model.SettingState
import com.nbtapps.store.model.SettingType
import com.nbtapps.store.model.SettingValue
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * The agent's rules for a value of a package's setting (its `agent/settings.hpp`, and `spec/settings.vectors`, which
 * both are tested against): what the phone checks before it sends anything, so that the agent seldom refuses. The
 * reasons are the agent's, as `settings.set` names them in the error `invalid`.
 */
object SettingRules {
    const val WRONG_TYPE = "wrong-type"
    const val OUT_OF_RANGE = "out-of-range"
    const val TOO_LONG = "too-long"
    const val BAD_CHARACTERS = "bad-characters"
    const val NOT_A_CHOICE = "not-a-choice"
    const val UNKNOWN_KEY = "unknown-key"

    /** The reasons this app knows. The agent may give one of a later version, which [refusal] shows with its code. */
    private val REASONS = setOf(WRONG_TYPE, OUT_OF_RANGE, TOO_LONG, BAD_CHARACTERS, NOT_A_CHOICE, UNKNOWN_KEY)

    /** What a `bool` option's switch stands for, as an input of [check]. */
    const val ON = "true"
    const val OFF = "false"

    /**
     * Why the agent would refuse [value], sent as JSON for [option], or null where it takes it:
     *
     *  - `bool` takes a boolean, `int` an integer from `min` to `max`, `text` and `select` a string
     *  - `text`: 1 to `maxLength` bytes of UTF-8, of the safe set, or with characters `any` with no control
     *    character and no space at either end. One that is too long and of characters the rule refuses is too long.
     *    With a characters rule of a later version, only no control character: the agent checks the rest.
     *  - `select`: one of its values, exactly
     *  - an option this app cannot read ([SettingType.UNKNOWN]) takes nothing from the phone
     */
    fun checkJson(option: SettingOption, value: JsonElement?): String? =
        reason(option, AgentClient.settingValue(option.type, value))

    /** [checkJson] for a value of the option's own kind; null for one of no kind at all. */
    fun reason(option: SettingOption, value: SettingValue?): String? = when (option.type) {
        SettingType.BOOL -> if (value is SettingValue.Bool) null else WRONG_TYPE
        SettingType.INT -> when {
            value !is SettingValue.Number -> WRONG_TYPE
            value.value !in option.min..option.max -> OUT_OF_RANGE
            else -> null
        }
        SettingType.TEXT -> when {
            value !is SettingValue.Text -> WRONG_TYPE
            value.value.isEmpty() || !wellFormed(value.value) -> BAD_CHARACTERS
            value.value.encodeToByteArray().size > option.maxLength -> TOO_LONG
            !ManifestParser.isSettingText(value.value, option.maxLength, option.anyCharacters, option.unknownCharacters) -> BAD_CHARACTERS
            else -> null
        }
        SettingType.SELECT -> when {
            value !is SettingValue.Text -> WRONG_TYPE
            option.values.none { it.value == value.value } -> NOT_A_CHOICE
            else -> null
        }
        SettingType.UNKNOWN -> WRONG_TYPE
    }

    /** The text the agent writes after `KEY=` for [value]: a bool as 1 or 0, an integer's digits, a string as it is. */
    fun line(value: SettingValue): String = when (value) {
        is SettingValue.Bool -> if (value.value) "1" else "0"
        is SettingValue.Number -> value.value.toString()
        is SettingValue.Text -> value.value
    }

    /**
     * What the user gave for [option] as the screen has it, [input]: [ON] or [OFF] for a switch, the digits typed
     * for a number, the text typed, the value picked. The value it is, and whether the agent would take it.
     */
    fun check(option: SettingOption, input: String): SettingCheck {
        val value = valueOf(option, input) ?: return SettingCheck.Refused(if (option.type == SettingType.INT && isDigits(input)) OUT_OF_RANGE else WRONG_TYPE)
        return reason(option, value)?.let { SettingCheck.Refused(it) } ?: SettingCheck.Ok(value)
    }

    /** What the user reads for [reason], one of these, about [option]; [input] is what they gave, where it is known. */
    fun explain(option: SettingOption, reason: String, input: String? = null): String = when (reason) {
        WRONG_TYPE -> when (option.type) {
            SettingType.BOOL -> "It isn't on or off"
            SettingType.INT -> "Enter a whole number"
            SettingType.TEXT -> "Enter some text"
            SettingType.SELECT -> "Pick one of the choices"
            SettingType.UNKNOWN -> "The head unit doesn't take this value"
        }
        OUT_OF_RANGE -> "Use a number from ${option.min} to ${option.max}"
        TOO_LONG -> if (option.anyCharacters || option.unknownCharacters) {
            "Too long: at most ${option.maxLength} bytes, and a letter such as é takes two"
        } else {
            "Too long: at most ${option.maxLength} characters"
        }
        BAD_CHARACTERS -> when {
            input?.isEmpty() == true -> "Enter some text"
            option.unknownCharacters -> "Some of these characters aren't allowed"
            option.anyCharacters -> "No space at the start or the end, and no control characters"
            else -> "Only letters A to Z, digits and . _ : / @ + , - are allowed"
        }
        NOT_A_CHOICE -> "Not one of the choices"
        UNKNOWN_KEY -> "The head unit doesn't know this setting"
        else -> "The head unit doesn't take this value"
    }

    /**
     * What the user reads under [option] when the head unit refused the value sent for it for [reason]: the reason in
     * words, or for a reason of a later version, which this app does not know, that it was refused and the reason's
     * code.
     */
    fun refusal(option: SettingOption, reason: String, input: String? = null): String =
        if (reason in REASONS) {
            "The head unit refused this: " + explain(option, reason, input).replaceFirstChar { it.lowercase() } + "."
        } else {
            "The head unit refused this value ($reason)."
        }

    /** [value] as an input of [check]: the opposite of what [check] makes of one. */
    fun input(value: SettingValue): String = when (value) {
        is SettingValue.Bool -> if (value.value) ON else OFF
        is SettingValue.Number -> value.value.toString()
        is SettingValue.Text -> value.value
    }

    private fun valueOf(option: SettingOption, input: String): SettingValue? = when (option.type) {
        SettingType.BOOL -> when (input) {
            ON -> SettingValue.Bool(true)
            OFF -> SettingValue.Bool(false)
            else -> null
        }
        // Digits that are too many for a Long are far outside any option's limits.
        SettingType.INT -> if (isDigits(input)) input.toLongOrNull()?.let { SettingValue.Number(it) } else null
        SettingType.TEXT, SettingType.SELECT -> SettingValue.Text(input)
        SettingType.UNKNOWN -> null
    }

    /** `-?[0-9]+`, as the agent reads a number. */
    private fun isDigits(input: String): Boolean {
        val digits = input.removePrefix("-")
        return digits.isNotEmpty() && digits.all { it in '0'..'9' }
    }

    /** No half of a surrogate pair on its own: text that is UTF-8 once encoded. */
    private fun wellFormed(text: String): Boolean {
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (c.isHighSurrogate() && i + 1 < text.length && text[i + 1].isLowSurrogate()) {
                i += 2
                continue
            }
            if (c.isSurrogate()) return false
            i++
        }
        return true
    }
}

/** What [SettingRules.check] made of an input. */
sealed interface SettingCheck {
    data class Ok(val value: SettingValue) : SettingCheck

    /** The agent would refuse it, for [reason]: one of [SettingRules]'s. */
    data class Refused(val reason: String) : SettingCheck
}

/**
 * Changes to a package's settings, as `settings.set` sends them: each key of [set] to its value, and each key of
 * [reset] out of the file. In the order of the package's options.
 */
data class SettingsChanges(val set: Map<String, SettingValue> = emptyMap(), val reset: List<String> = emptyList()) {
    val keys: Set<String> get() = set.keys + reset

    val isEmpty: Boolean get() = set.isEmpty() && reset.isEmpty()

    /**
     * These changes as requests that each fit a frame of [maxFrame] bytes on the link, with any request id: one where
     * they all fit, and otherwise as few as the order allows, the sets before the resets. The agent takes each request
     * whole or not at all, not the parts together.
     *
     * @throws IllegalArgumentException where one change alone does not fit, which no option the manifest's limits
     *   allow does on a frame of 4,096 bytes.
     */
    fun parts(packageId: String, maxFrame: Int): List<SettingsChanges> {
        val parts = ArrayList<SettingsChanges>()
        var current = SettingsChanges()
        fun add(next: SettingsChanges) {
            if (current.isEmpty || requestBytes(packageId, next) <= maxFrame) {
                current = next
                return
            }
            parts += current
            current = SettingsChanges()
        }
        for ((key, value) in set) {
            add(current.copy(set = current.set + (key to value)))
            if (key !in current.set) add(SettingsChanges(set = mapOf(key to value)))
        }
        for (key in reset) {
            add(current.copy(reset = current.reset + key))
            if (key !in current.reset) add(SettingsChanges(reset = listOf(key)))
        }
        if (!current.isEmpty) parts += current
        for (part in parts) {
            require(requestBytes(packageId, part) <= maxFrame) { "a change of ${part.keys.single()} does not fit a frame of $maxFrame bytes" }
        }
        return parts
    }

    companion object {
        /** The largest request id the phone can send, which has the most digits: what a request is measured with. */
        private const val LONGEST_ID = 9_007_199_254_740_991L

        /** The bytes of the request `settings.set` of [changes] as it goes in a frame, with the longest id. */
        fun requestBytes(packageId: String, changes: SettingsChanges): Int {
            val request = buildJsonObject {
                put("id", LONGEST_ID)
                put("method", "settings.set")
                put("params", AgentClient.settingsSetParams(packageId, changes.set, changes.reset))
            }
            return Json.encodeToString(JsonObject.serializer(), request).encodeToByteArray().size
        }
    }
}

/**
 * A package's settings as the screen edits them: [entries] as they were read from the head unit, [inputs] for the
 * options the user has given something since (what was typed or picked, exactly, checked or not: see
 * [SettingRules.check]), and [resets] for those taken back to their default. An option is never in both.
 *
 * What a field holds and whether it is a change are two things. A field keeps what was typed into it, always: an
 * option not set whose default is 10 holds "1", "10" and "100" in turn as they are typed. But an input that comes to
 * what the option was as it was read is no change: it is not counted, not sent, and not shown as a change. For an
 * option not set, that is no value or its default's, so a default cannot be pinned in the file by typing it.
 */
data class SettingsEdit(
    val entries: List<SettingEntry>,
    val inputs: Map<String, String> = emptyMap(),
    val resets: Set<String> = emptySet(),
) {
    fun entry(key: String): SettingEntry? = entries.firstOrNull { it.option.key == key }

    /** The options that have a change waiting: an input that is not how the option was read, or a reset. */
    val changed: Set<String> get() = inputs.keys.filterTo(LinkedHashSet()) { isChange(it) } + resets

    val count: Int get() = changed.size

    /** Whether the input of [key] is a change: one the option, as read, does not come to already. */
    fun isChange(key: String): Boolean {
        val input = inputs[key] ?: return false
        val entry = entry(key) ?: return false
        return !asRead(entry, input)
    }

    /**
     * Gives the option [key] the value [input], as the field has it now. An input that is just what the field showed
     * as read (the value, the text set by hand, empty for one not set, a switch's position) leaves no input at all,
     * as the field shows that anyway. An option shown read-only ([SettingEntry.readOnly]) takes none.
     */
    fun set(key: String, input: String): SettingsEdit {
        val entry = entry(key)?.takeIf { !it.readOnly } ?: return this
        return if (input == shownAsRead(entry)) copy(inputs = inputs - key, resets = resets - key) else copy(inputs = inputs + (key to input), resets = resets - key)
    }

    /**
     * Takes the option [key] back to its default: its line goes from the file. One that is not set stays so. An option
     * shown read-only can be reset too, since only its key is sent.
     */
    fun reset(key: String): SettingsEdit {
        val entry = entry(key) ?: return this
        return copy(inputs = inputs - key, resets = if (entry.state == SettingState.UNSET) resets - key else resets + key)
    }

    /** Every change dropped. */
    fun discard(): SettingsEdit = copy(inputs = emptyMap(), resets = emptySet())

    /**
     * After [sent] was saved, of which the head unit wrote the options [keys]: their entries are as it has them now,
     * set to the value sent or not set, whether or not they are read again. A change of one of them is dropped only
     * where it is still what was sent; one made while the save went on stays, over the new entry.
     */
    fun saved(sent: SettingsChanges, keys: Set<String> = sent.keys): SettingsEdit {
        val fresh = entries.map { entry ->
            val key = entry.option.key
            val value = sent.set[key]
            when {
                key !in keys -> entry
                value != null -> SettingEntry(entry.option, SettingState.SET, value = value)
                key in sent.reset -> SettingEntry(entry.option, SettingState.UNSET)
                else -> entry
            }
        }
        val kept = inputs.filterNot { (key, input) ->
            val value = sent.set[key]
            val option = entry(key)?.option
            key in keys && value != null && option != null && SettingRules.check(option, input) == SettingCheck.Ok(value)
        }
        // A reset that was sent is of an option not set now, which over() drops; a reset asked for during the save of
        // a value that it set stays.
        return copy(inputs = kept).over(fresh)
    }

    /** The options whose change the agent would refuse, and why: one of [SettingRules]'s reasons for each. */
    val problems: Map<String, String>
        get() = inputs.mapNotNull { (key, input) ->
            val entry = entry(key)?.takeIf { !asRead(it, input) } ?: return@mapNotNull null
            (SettingRules.check(entry.option, input) as? SettingCheck.Refused)?.let { key to it.reason }
        }.toMap()

    /**
     * The changes as `settings.set` sends them, in the order of the options. Null while an input would be refused. An
     * option shown read-only is never given a value, only reset.
     */
    fun changes(): SettingsChanges? {
        val set = LinkedHashMap<String, SettingValue>()
        for (entry in entries) {
            if (entry.readOnly) continue
            val input = inputs[entry.option.key]?.takeIf { !asRead(entry, it) } ?: continue
            set[entry.option.key] = (SettingRules.check(entry.option, input) as? SettingCheck.Ok)?.value ?: return null
        }
        return SettingsChanges(set, entries.map { it.option.key }.filter { it in resets })
    }

    /**
     * The same inputs and resets over [fresh] entries, read again from the head unit: those of options it no longer
     * has are dropped, and those that it has now anyway are no longer changes.
     */
    fun over(fresh: List<SettingEntry>): SettingsEdit {
        var edit = SettingsEdit(fresh)
        for ((key, input) in inputs) edit = edit.set(key, input)
        for (key in resets) edit = edit.reset(key)
        return edit
    }

    private companion object {
        /**
         * What the option's field shows as read, as an input: the value, the text set by hand, and for one not set
         * nothing, except a switch, which is at its default or off, and a choice, which shows its default.
         */
        fun shownAsRead(entry: SettingEntry): String = when (entry.state) {
            // Of an option this app cannot read, set to a value it cannot read, nothing is shown.
            SettingState.SET -> entry.value?.let { SettingRules.input(it) }.orEmpty()
            SettingState.BY_HAND -> entry.raw.orEmpty()
            SettingState.UNSET -> when (entry.option.type) {
                SettingType.BOOL -> entry.option.default?.let { SettingRules.input(it) } ?: SettingRules.OFF
                SettingType.SELECT -> entry.option.default?.let { SettingRules.input(it) }.orEmpty()
                SettingType.INT, SettingType.TEXT, SettingType.UNKNOWN -> ""
            }
            SettingState.UNKNOWN -> ""
        }

        /**
         * Whether [input] comes to what the option was as read: the same value, the text set by hand itself, or for
         * one not set nothing, its default, or a switch left where it showed.
         */
        fun asRead(entry: SettingEntry, input: String): Boolean {
            if (input == shownAsRead(entry)) return true
            val value = (SettingRules.check(entry.option, input) as? SettingCheck.Ok)?.value
            return when (entry.state) {
                SettingState.SET -> value != null && value == entry.value
                SettingState.UNSET -> input.isEmpty() || (value != null && value == entry.option.default)
                SettingState.BY_HAND, SettingState.UNKNOWN -> false
            }
        }
    }
}

/** Why the settings of a package could not be read or saved, as the screen tells the user. */
sealed interface SettingsFailure {
    val message: String

    /** There is no link to the head unit. */
    data object NotConnected : SettingsFailure {
        override val message = "Not connected to the head unit"
    }

    /** The agent has no settings: one from before them, or a link whose frame is too small for them. */
    data object TooOld : SettingsFailure {
        override val message = "This head unit's agent can't change settings. Update the agent first."
    }

    /** A job runs on the head unit: while it does, nothing is changed, and while an install step runs nothing is read. */
    data object Busy : SettingsFailure {
        override val message = "An install is running on the head unit. Settings can be read and saved when it has finished."
    }

    /** The package is not installed on the head unit any more, or names no settings. */
    data object NotFound : SettingsFailure {
        override val message = "The head unit no longer has this package, or the package has no settings."
    }

    /** The agent would not take some of the values: [faults] says which and why. Nothing of that request was written. */
    data class Invalid(val faults: List<SettingFault>) : SettingsFailure {
        override val message = if (faults.size == 1) "The head unit refused 1 change" else "The head unit refused ${faults.size} changes"
    }

    /** The link ended, or failed, before the answer came. */
    data class LinkLost(override val message: String) : SettingsFailure

    /** Anything else, in the app's words. */
    data class Failed(override val message: String) : SettingsFailure
}

/**
 * What a read or a save of settings fails with. [saved] names the options of a save in parts that were saved before
 * a later part failed: those are written on the head unit, the rest are not.
 */
class SettingsException(val failure: SettingsFailure, val saved: Set<String> = emptySet()) : Exception(failure.message)

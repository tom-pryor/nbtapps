// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.settings

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.settings.SettingRules
import com.nbtapps.store.data.settings.SettingsEdit
import com.nbtapps.store.data.settings.SettingsException
import com.nbtapps.store.data.settings.SettingsFailure
import com.nbtapps.store.model.SettingApplies
import com.nbtapps.store.model.SettingChoice
import com.nbtapps.store.model.SettingEntry
import com.nbtapps.store.model.SettingOption
import com.nbtapps.store.model.SettingState
import com.nbtapps.store.model.SettingType
import com.nbtapps.store.model.SettingValue
import com.nbtapps.store.ui.navigation.SettingsRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.flow.updateAndGet
import kotlinx.coroutines.launch

/** Where the reading of the settings from the head unit stands, while the screen has none to show. */
sealed interface SettingsLoad {
    data object Loading : SettingsLoad

    data class Failed(val failure: SettingsFailure) : SettingsLoad

    data object Done : SettingsLoad
}

/** One line of the saved card: how many of the changes saved take effect when. */
data class SavedGroup(val applies: SettingApplies, val count: Int)

/**
 * One option as its row shows it: label and description, the field, and the line under it with how it stands, its
 * default and its limits, and Reset at that line's right where the option has something to reset.
 */
data class OptionRow(
    val key: String,
    val label: String,
    val description: String?,
    val type: SettingType,
    /** What the field holds: the change typed or picked, the value read, or the raw text of one set by hand. */
    val input: String,
    /** With no value of its own, what the field shows instead, muted: the default, or what the manifest says then. */
    val placeholder: String?,
    /** A switch's position as shown: the value, or with none the default. */
    val on: Boolean,
    /** The field shows nothing of the option's own: the option is not set, or is to be reset. */
    val muted: Boolean,
    /** The head unit holds a value set by hand that the option's type does not take, and no change replaces it. */
    val byHand: Boolean,
    /** A change of this option waits to be saved. */
    val changed: Boolean,
    /** Why the value would not be taken, or was not: the phone's check, or the head unit's refusal. */
    val error: String?,
    val meta: String,
    val canReset: Boolean,
    val choices: List<SettingChoice>,
    val unit: String?,
    /** When a change takes effect, where the options do not all say the same. Null where they do. */
    val takesEffect: String?,
    val advanced: Boolean,
)

data class SettingsUiState(
    val packageId: String,
    /** The package's name, as the head unit lists it, or its id. */
    val name: String,
    /** There is a link to this head unit. */
    val connected: Boolean = false,
    /** There is a link, and its agent has no settings. */
    val tooOld: Boolean = false,
    /** A job runs on this head unit: nothing can be saved until it has finished. */
    val jobRunning: Boolean = false,
    val load: SettingsLoad = SettingsLoad.Loading,
    /** The settings as read, and the changes not saved yet. Null until they have been read once. */
    val edit: SettingsEdit? = null,
    /** The options the head unit refused at the last save, with the reason it gave. */
    val refused: Map<String, String> = emptyMap(),
    val saving: Boolean = false,
    /** What the last save saved, by when it takes effect: the card at the top. Null before a save. */
    val saved: List<SavedGroup>? = null,
    /** Why the last save failed, where no option's row says so. */
    val error: String? = null,
    val advancedOpen: Boolean = false,
    val rows: List<OptionRow> = emptyList(),
    /** When every change takes effect, where the options all say the same. */
    val takesEffect: String? = null,
) {
    val count: Int get() = edit?.count ?: 0

    /** The link went while there are settings on the screen: they and the changes stay, and nothing is saved. */
    val linkLost: Boolean get() = edit != null && !connected

    val canSave: Boolean
        get() = connected && !tooOld && !jobRunning && !saving && count > 0 && edit?.problems.orEmpty().isEmpty()

    val advancedChanged: Int get() = rows.count { it.advanced && it.changed }
}

/**
 * The settings of one installed package on one head unit: read from the head unit when the screen opens (and again
 * once a link to it is back), changed on the phone, checked there by the agent's rules, and saved at once. Nothing is
 * saved while the head unit runs a job, or without a link; the changes made meanwhile stay on the screen.
 */
@HiltViewModel
class SettingsViewModel internal constructor(
    private val headUnitId: String,
    private val packageId: String,
    private val jobs: JobRepository,
    headUnits: HeadUnitRepository,
) : ViewModel() {

    @Inject
    constructor(savedStateHandle: SavedStateHandle, jobs: JobRepository, headUnits: HeadUnitRepository) : this(
        savedStateHandle.toRoute<SettingsRoute>().headUnitId,
        savedStateHandle.toRoute<SettingsRoute>().packageId,
        jobs,
        headUnits,
    )

    private data class Local(
        val load: SettingsLoad = SettingsLoad.Loading,
        val edit: SettingsEdit? = null,
        val refused: Map<String, String> = emptyMap(),
        val saving: Boolean = false,
        val saved: List<SavedGroup>? = null,
        val error: String? = null,
        val advancedOpen: Boolean = false,
        /**
         * Counts the reads begun, and the saves that wrote something: a read's answer is taken only while nothing has
         * been counted since it began.
         */
        val reads: Long = 0,
    )

    private val local = MutableStateFlow(Local())

    /** This head unit's link, when it is one to an agent that has settings. */
    private fun ConnectionState.mine(): ConnectionState.Connected? = (this as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }

    val uiState: StateFlow<SettingsUiState> =
        combine(jobs.connection, jobs.job, headUnits.headUnit(headUnitId), local) { connection, job, unit, local ->
            val connected = connection.mine()
            val installed = connected?.installed ?: unit?.profile?.installed.orEmpty()
            val name = installed.firstOrNull { it.id == packageId }?.name ?: packageId
            val edit = local.edit
            val sameEffect = edit?.entries?.map { it.option.applies }?.distinct()?.singleOrNull()
            SettingsUiState(
                packageId = packageId,
                name = name,
                connected = connected != null,
                tooOld = connected != null && !connected.settings,
                jobRunning = job != null && job.headUnitId == headUnitId && !job.ended && !job.forgotten,
                load = local.load,
                edit = edit,
                refused = local.refused,
                saving = local.saving,
                saved = local.saved,
                error = local.error,
                advancedOpen = local.advancedOpen,
                rows = edit?.let { optionRows(it, local.refused, name, mixed = sameEffect == null) }.orEmpty(),
                takesEffect = sameEffect?.let { takesEffect(it, name) },
            )
        }.stateIn(viewModelScope, SharingStarted.Eagerly, SettingsUiState(packageId, packageId))

    init {
        // Read when a link to an agent with settings is there, at once or later; read again when one is back.
        viewModelScope.launch {
            jobs.connection.map { it.mine()?.settings == true }.distinctUntilChanged().collect { up -> if (up) load() }
        }
        // Settings that could not be read during an install are read once it has finished.
        viewModelScope.launch {
            jobs.job.map { it != null && it.headUnitId == headUnitId && !it.ended }.distinctUntilChanged().collect { running ->
                if (!running && (local.value.load as? SettingsLoad.Failed)?.failure == SettingsFailure.Busy) load()
            }
        }
    }

    /**
     * Reads the settings from the head unit. Changes on the screen are kept, over what is read now: those that the
     * head unit has anyway are no longer changes.
     *
     * Reads may overlap, as a link comes back while one is under way. Only the answer of the last one begun is
     * taken, and not that of one begun before a save wrote something: either could come late and put back what was
     * there before.
     */
    fun load() {
        val mine = local.updateAndGet { it.copy(reads = it.reads + 1, load = if (it.edit == null) SettingsLoad.Loading else it.load) }.reads
        viewModelScope.launch {
            val result = jobs.loadSettings(headUnitId, packageId)
            local.update { state ->
                if (state.reads != mine) return@update state
                result.fold(
                    onSuccess = { entries ->
                        state.copy(load = SettingsLoad.Done, edit = state.edit?.over(entries) ?: SettingsEdit(entries))
                    },
                    onFailure = { failure ->
                        val why = (failure as? SettingsException)?.failure ?: SettingsFailure.Failed(failure.message ?: "The head unit didn't answer")
                        // Settings already on the screen stay, with their changes.
                        if (state.edit != null) state else state.copy(load = SettingsLoad.Failed(why))
                    },
                )
            }
        }
    }

    /** Gives the option [key] the value [input]: see [SettingRules.check]. */
    fun set(key: String, input: String) = local.update { it.copy(edit = it.edit?.set(key, input), refused = it.refused - key) }

    /** Takes the option [key] back to not set, so that the package goes by its default. */
    fun reset(key: String) = local.update { it.copy(edit = it.edit?.reset(key), refused = it.refused - key) }

    /** Drops every change not saved. */
    fun discard() = local.update { it.copy(edit = it.edit?.discard(), refused = emptyMap(), error = null) }

    fun toggleAdvanced() = local.update { it.copy(advancedOpen = !it.advancedOpen) }

    /**
     * Saves the changes. Those saved leave the screen's list of changes, unless they were changed again while the
     * save went on; the rows show the values saved at once, the card at the top says when they take effect, and the
     * settings are read again. A refusal is shown on the options at fault, the other changes kept.
     */
    fun save() {
        if (!uiState.value.canSave) return
        val edit = local.value.edit ?: return
        val changes = edit.changes() ?: return
        local.update { it.copy(saving = true, error = null, refused = emptyMap()) }
        viewModelScope.launch {
            val failure = jobs.saveSettings(headUnitId, packageId, changes).exceptionOrNull()
            val saved = if (failure == null) changes.keys else (failure as? SettingsException)?.saved.orEmpty()
            local.update { state ->
                state.copy(
                    saving = false,
                    // What was saved is on the screen even where the read that follows fails.
                    edit = if (saved.isEmpty()) state.edit else state.edit?.saved(changes, saved),
                    reads = if (saved.isEmpty()) state.reads else state.reads + 1,
                    saved = if (saved.isEmpty()) state.saved else savedGroups(edit, saved),
                    refused = when (val why = (failure as? SettingsException)?.failure) {
                        is SettingsFailure.Invalid -> why.faults.associate { it.key to it.reason }
                        else -> emptyMap()
                    },
                    // The link's end, where nothing was saved, is the link-lost banner's to tell.
                    error = failure?.let { saveError(it, saved.size, changes.keys.size) },
                    // An option at fault among the advanced ones is shown.
                    advancedOpen = state.advancedOpen || (failure as? SettingsException)?.failure.let { why ->
                        why is SettingsFailure.Invalid && why.faults.any { fault -> edit.entry(fault.key)?.option?.advanced == true }
                    },
                )
            }
            if (saved.isNotEmpty()) load()
        }
    }

    private fun saveError(failure: Throwable, saved: Int, total: Int): String? {
        val why = (failure as? SettingsException)?.failure
        if (why is SettingsFailure.LinkLost && saved == 0) return null
        val before = if (saved > 0) "$saved of $total changes were saved. " else ""
        return before + when (why) {
            is SettingsFailure.Invalid ->
                (if (saved > 0) "" else "Nothing was saved: ") +
                    (if (why.faults.size == 1) "the head unit refused one change. Correct it" else "the head unit refused ${why.faults.size} changes. Correct them") +
                    " and save again; your other changes are kept."
            SettingsFailure.Busy -> "An install is running on the head unit. Save again when it has finished; your changes are kept."
            is SettingsFailure.LinkLost -> "The link to the head unit was lost. Your changes are kept here; connect again to save them."
            null -> failure.message ?: "The head unit didn't answer"
            else -> why.message
        }.replaceFirstChar { it.uppercase() }
    }
}

/** [count] changes take effect when [applies] says, for a package called [name]: a line of the saved card. */
internal fun savedLine(group: SavedGroup, name: String): String = when (group.applies) {
    SettingApplies.NOW -> if (group.count == 1) "1 change is in effect now." else "${group.count} changes are in effect now."
    else -> (if (group.count == 1) "1 change takes effect " else "${group.count} changes take effect ") + takesEffect(group.applies, name) + "."
}

/** When a change of an option with [applies] takes effect, for a package called [name]. */
internal fun takesEffect(applies: SettingApplies, name: String): String = when (applies) {
    SettingApplies.NOW -> "at once"
    SettingApplies.APP_START -> "when $name is next started"
    SettingApplies.HMI_RESTART -> "when iDrive next restarts"
    SettingApplies.REBOOT -> "when the head unit next restarts"
}

/** The options of [keys] that were saved, counted by when they take effect, in the order of [SettingApplies]. */
internal fun savedGroups(edit: SettingsEdit, keys: Set<String>): List<SavedGroup> =
    edit.entries.filter { it.option.key in keys }.groupingBy { it.option.applies }.eachCount()
        .toSortedMap().map { (applies, count) -> SavedGroup(applies, count) }

/** [value] in words, as an option of its kind is shown: on or off, a number with its unit, a choice's label. */
internal fun shown(option: SettingOption, value: SettingValue): String = when (value) {
    is SettingValue.Bool -> if (value.value) "on" else "off"
    is SettingValue.Number -> listOfNotNull(value.value.toString(), option.unit).joinToString(" ")
    is SettingValue.Text -> option.values.firstOrNull { it.value == value.value }?.label ?: value.value
}

/** What the option comes to with no value of its own: its default in words, or what the manifest says then. */
private fun fallback(option: SettingOption): String? = option.default?.let { shown(option, it) } ?: option.unset

/** The option's limits, as its row's line ends with them. */
private fun limits(option: SettingOption): String? = when (option.type) {
    SettingType.INT -> "${option.min} to ${option.max}"
    SettingType.TEXT -> if (option.anyCharacters) "up to ${option.maxLength} bytes" else "up to ${option.maxLength} characters"
    else -> null
}

/** The rows of [edit]'s options, with the head unit's [refused] reasons; [mixed] where they take effect at different times. */
internal fun optionRows(edit: SettingsEdit, refused: Map<String, String>, name: String, mixed: Boolean): List<OptionRow> {
    val problems = edit.problems
    return edit.entries.map { entry -> optionRow(entry, edit, problems[entry.option.key], refused[entry.option.key], name, mixed) }
}

private fun optionRow(entry: SettingEntry, edit: SettingsEdit, problem: String?, refused: String?, name: String, mixed: Boolean): OptionRow {
    val option = entry.option
    val key = option.key
    // What was typed or picked: the field keeps it, whether or not it is a change.
    val pending = edit.inputs[key]
    val changing = edit.isChange(key)
    val resetting = key in edit.resets
    val read = entry.value?.let { shown(option, it) }
    val ownInput = when {
        pending != null -> pending
        resetting -> ""
        entry.state == SettingState.SET -> SettingRules.input(requireNotNull(entry.value))
        entry.state == SettingState.BY_HAND -> entry.raw.orEmpty()
        else -> ""
    }
    val byHand = pending == null && !resetting && entry.state == SettingState.BY_HAND
    val muted = pending == null && (resetting || entry.state == SettingState.UNSET)
    val fallback = fallback(option)
    val status = when {
        changing -> "Changed · was " + when (entry.state) {
            SettingState.SET -> read
            SettingState.UNSET -> "not set"
            SettingState.BY_HAND -> "set by hand"
        }
        resetting -> "Changed · back to not set" + (option.default?.let { " · uses ${shown(option, it)}" } ?: option.unset?.let { " · $it" }).orEmpty()
        entry.state == SettingState.UNSET -> "Not set" + (option.default?.let { " · uses ${shown(option, it)}" } ?: option.unset?.let { " · $it" }).orEmpty()
        else -> fallback?.let { "Default: $it" } ?: "No default"
    }
    val error = when {
        problem != null -> SettingRules.explain(option, problem, pending)
        refused != null -> "The head unit refused this: " + SettingRules.explain(option, refused, pending).replaceFirstChar { it.lowercase() } + "."
        else -> null
    }
    return OptionRow(
        key = key,
        label = option.label,
        description = option.description,
        type = option.type,
        input = ownInput,
        // A number's unit is the field's own suffix, so its default goes in without it.
        placeholder = if (ownInput.isEmpty()) (option.default as? SettingValue.Number)?.value?.toString() ?: fallback else null,
        on = when {
            pending != null -> pending == SettingRules.ON
            !muted && entry.value is SettingValue.Bool -> (entry.value as SettingValue.Bool).value
            else -> (option.default as? SettingValue.Bool)?.value == true
        },
        muted = muted,
        byHand = byHand,
        changed = changing || resetting,
        error = error,
        meta = listOfNotNull(status, limits(option)).joinToString(" · "),
        canReset = (entry.state != SettingState.UNSET && !resetting) || changing,
        choices = option.values,
        unit = option.unit,
        takesEffect = if (mixed) "Takes effect " + takesEffect(option.applies, name) else null,
        advanced = option.advanced,
    )
}

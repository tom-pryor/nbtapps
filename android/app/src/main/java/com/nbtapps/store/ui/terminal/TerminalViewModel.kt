// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.terminal

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.TextFieldValue
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.terminal.TerminalSnapshot
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.ChannelSwitch
import com.nbtapps.store.ui.detail.sshSwitchError
import com.nbtapps.store.ui.navigation.TerminalRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlin.math.floor
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** What the terminal screen shows of the terminal on its head unit, besides the lines. */
sealed interface TerminalView {
    /** The link is being made, or the shell opened. */
    data object Opening : TerminalView

    /** The shell is open; [held] while a job runs on the head unit. */
    data class Open(val held: Boolean) : TerminalView

    /** The shell has ended: the lines end with a line that says how. */
    data object Ended : TerminalView

    /** No shell could be had, for [reason]. */
    data class Failed(val reason: String) : TerminalView

    /** SSH access is switched off on the head unit. */
    data object Off : TerminalView

    /** The SSH package is not on the head unit. */
    data object NoPackage : TerminalView

    /** The head unit's agent has no channels. */
    data object Unsupported : TerminalView

    /** A terminal is open, or being opened, on another head unit: the app has one at a time. */
    data object Elsewhere : TerminalView

    /**
     * The terminal that was open on another head unit ([Elsewhere]) has ended. None is opened here by itself: the user
     * opens one.
     */
    data object OtherClosed : TerminalView
}

/**
 * What the screen shows for the terminal's [state], on the head unit [headUnitId]. The screen opens a shell as it comes
 * up, which makes the state this head unit's at once unless one is open on another ([TerminalViewModel]): so another
 * head unit's state that is not active is met only once that one has ended.
 */
internal fun terminalView(state: TerminalState, headUnitId: String): TerminalView {
    if (state == TerminalState.Idle) return TerminalView.Opening
    if (state.headUnitId != headUnitId) return if (state.active) TerminalView.Elsewhere else TerminalView.OtherClosed
    return when (state) {
        TerminalState.Idle, is TerminalState.Connecting -> TerminalView.Opening
        is TerminalState.Open -> TerminalView.Open(state.held)
        is TerminalState.Ended -> TerminalView.Ended
        is TerminalState.Failed -> TerminalView.Failed(state.reason)
        is TerminalState.Off -> TerminalView.Off
        is TerminalState.NoPackage -> TerminalView.NoPackage
        is TerminalState.Unsupported -> TerminalView.Unsupported
    }
}

/** A pty's size, in characters. */
data class TerminalGrid(val columns: Int, val rows: Int)

/**
 * The pty's size for a lines area of [width] by [height] pixels, inside its padding, where one character takes
 * [cellWidth] by [cellHeight] pixels: as many whole characters as fit each way, at least one column and at least
 * [MIN_ROWS] rows. The terminal keeps lines, not a grid, so rows beyond those that show do no harm, where a program
 * told of one or two would draw nothing usable. Null where the cell has no size yet.
 */
fun terminalGrid(width: Int, height: Int, cellWidth: Float, cellHeight: Float): TerminalGrid? {
    if (cellWidth <= 0f || cellHeight <= 0f) return null
    val columns = floor(width.coerceAtLeast(0) / cellWidth).toInt().coerceAtLeast(1)
    val rows = floor(height.coerceAtLeast(0) / cellHeight).toInt().coerceAtLeast(MIN_ROWS)
    return TerminalGrid(columns, rows)
}

/** The fewest rows the shell is told of: see [terminalGrid]. */
const val MIN_ROWS = 4

/**
 * Whether the terminal takes its compact layout: the screen is [heightDp] high, of which [topDp] at the top and
 * [bottomDp] at the foot are the system's bars, and [imeDp] at the foot the keyboard's. Compact where the room left is
 * under [COMPACT_BELOW_DP], as in landscape with the keyboard up, on a small phone or in a split screen; it is the
 * height there is that decides, not the orientation.
 */
fun compactTerminal(heightDp: Float, topDp: Float, bottomDp: Float, imeDp: Float): Boolean =
    heightDp - topDp - maxOf(bottomDp, imeDp) < COMPACT_BELOW_DP

/**
 * The room under which the terminal is compact, in dp. The normal layout's top bar, key row and Command row take about
 * 195 dp; with this much room the lines keep about 6 rows above them, and with less they would be squeezed to a few
 * or none.
 */
const val COMPACT_BELOW_DP = 320f

data class TerminalUiState(
    /** The head unit's name, under the title. Null until it has been read. */
    val name: String? = null,
    val view: TerminalView = TerminalView.Opening,
    val lines: TerminalSnapshot = TerminalSnapshot(emptyList(), 0),
    /** The question before SSH access is switched on is open. */
    val confirmingOn: Boolean = false,
    /** SSH access is being switched on, and the agent has not answered yet. */
    val switchingOn: Boolean = false,
    /** Why the agent did not switch SSH access on. */
    val switchError: String? = null,
) {
    /** What is typed, the keys and the field may be used: the shell is open and not held. */
    val canType: Boolean get() = view == TerminalView.Open(held = false)

    /** The top bar has its actions: there are lines, or a shell being opened or open. */
    val hasActions: Boolean get() = view == TerminalView.Opening || view is TerminalView.Open || view == TerminalView.Ended

    /** A shell is being opened or is open: Close ends it. */
    val canClose: Boolean get() = view == TerminalView.Opening || view is TerminalView.Open
}

/** The bytes the control keys send. */
object ControlKey {
    const val CTRL_C: Byte = 0x03
    const val CTRL_D: Byte = 0x04
    const val CTRL_Z: Byte = 0x1A
}

/** What the screen has of lines that are another head unit's: none. */
private val NO_LINES = TerminalSnapshot(emptyList(), -1)

/** What Send puts after the field's text: Enter, as a terminal's key sends it. */
internal const val ENTER = "\r"

/**
 * The terminal on one head unit ([TerminalRoute]): the app's terminal ([TerminalRepository]) as a screen shows it, with
 * the field, its history and the size of the lines area.
 *
 * The screen coming up opens a shell, unless one is open or being opened on this head unit. Leaving the screen leaves
 * the shell open: it is the app's, held under the notification.
 */
@HiltViewModel
class TerminalViewModel internal constructor(
    val headUnitId: String,
    private val terminal: TerminalRepository,
    private val history: CommandHistory,
    name: Flow<String?>,
    /** Switches SSH access on the head unit on or off, over the link to it: [JobRepository.setChannelSwitch]. */
    private val setSwitch: suspend (on: Boolean) -> Result<ChannelSwitch>,
) : ViewModel() {

    @Inject
    constructor(
        savedStateHandle: SavedStateHandle,
        terminal: TerminalRepository,
        history: CommandHistory,
        headUnits: HeadUnitRepository,
        jobs: JobRepository,
    ) : this(
        savedStateHandle.toRoute<TerminalRoute>().headUnitId,
        terminal,
        history,
        headUnits.headUnit(savedStateHandle.toRoute<TerminalRoute>().headUnitId).map { it?.name },
        { on -> jobs.setChannelSwitch(savedStateHandle.toRoute<TerminalRoute>().headUnitId, SshTunnel.PACKAGE, SshTunnel.CHANNEL, on) },
    )

    private data class Local(val confirmingOn: Boolean = false, val switchingOn: Boolean = false, val switchError: String? = null)

    private val local = MutableStateFlow(Local())

    // Before [uiState] is made: the open is synchronous, so that the screen's first state is already this head unit's
    // and never another's that has ended ([TerminalView.OtherClosed]).
    init {
        val now = terminal.state.value
        if (!(now.active && now.headUnitId == headUnitId)) open()
    }

    val uiState: StateFlow<TerminalUiState> =
        combine(terminal.state, terminal.lines, name, local) { state, lines, name, local ->
            TerminalUiState(name, terminalView(state, headUnitId), ownLines(state, lines), local.confirmingOn, local.switchingOn, local.switchError)
        }.stateIn(
            viewModelScope,
            SharingStarted.WhileSubscribed(5_000),
            terminal.state.value.let { state ->
                TerminalUiState(view = terminalView(state, headUnitId), lines = ownLines(state, terminal.lines.value))
            },
        )

    /** [lines] where they are of this head unit's shells, as [state] says; none where they are another's. */
    private fun ownLines(state: TerminalState, lines: TerminalSnapshot): TerminalSnapshot =
        if (state.headUnitId == headUnitId) lines else NO_LINES

    /** The lines are of this head unit's shells: Clear and Copy all act on them. */
    private fun ownsLines(): Boolean = terminal.state.value.headUnitId == headUnitId

    /** The field: what the user is typing, or a line brought back from the history. */
    var field by mutableStateOf(TextFieldValue(""))
        private set

    /** Opens a shell: as the screen comes up, and from Open again and Try again. */
    fun open() {
        local.update { it.copy(switchError = null) }
        // The size is the last one the screen gave, or the repository's own until it gives one.
        terminal.open(headUnitId, 0, 0)
    }

    /** The field changed as the user typed: an edit of its text begins the history's walk again ([CommandHistory.edited]). */
    fun type(value: TextFieldValue) {
        if (value.text != field.text) history.edited(value.text)
        field = value
    }

    /** Sends the field's text and Enter, and empties the field; an empty field sends Enter alone. The shell's echo shows it. */
    fun send() {
        if (!canType()) return
        val text = field.text
        terminal.send((text + ENTER).encodeToByteArray())
        history.sent(text)
        field = TextFieldValue("")
    }

    /** Sends one control key's byte at once, whatever the field holds. */
    fun key(byte: Byte) {
        if (!canType()) return
        terminal.send(byteArrayOf(byte))
    }

    /** Up: the line before the one in the field. */
    fun earlier() {
        if (!canType()) return
        history.earlier(field.text)?.let(::recall)
    }

    /** Down: the line after the one in the field, or what the user was typing. */
    fun later() {
        if (!canType()) return
        history.later()?.let(::recall)
    }

    private fun recall(text: String) {
        field = TextFieldValue(text, TextRange(text.length))
    }

    private fun canType(): Boolean = terminalView(terminal.state.value, headUnitId) == TerminalView.Open(held = false)

    /** The lines area is [grid] in characters now: the shell is told, once the size has settled. */
    fun sized(grid: TerminalGrid) = terminal.resize(grid.columns, grid.rows)

    /** Close: ends the shell, or gives up opening one. The screen stays, with the lines. */
    fun close() {
        if (ownsLines()) terminal.close()
    }

    /** Clear: empties the lines but the one the shell is writing. Nothing of another head unit's. */
    fun clear() {
        if (ownsLines()) terminal.clear()
    }

    /** Copy all: every line, as the clipboard is to have it. Empty where the lines are another head unit's. */
    fun copyText(): String = if (ownsLines()) terminal.text() else ""

    /** Switch on, where SSH access is off: asks first ([confirmOn]). */
    fun askOn() {
        if (!local.value.switchingOn) local.update { it.copy(confirmingOn = true) }
    }

    fun cancelOn() = local.update { it.copy(confirmingOn = false) }

    /** The question is answered yes: SSH access is switched on, and a shell opened. */
    fun confirmOn() {
        if (!local.value.confirmingOn) return
        local.update { it.copy(confirmingOn = false, switchingOn = true, switchError = null) }
        viewModelScope.launch {
            val result = setSwitch(true)
            local.update { it.copy(switchingOn = false, switchError = result.exceptionOrNull()?.let(::sshSwitchError)) }
            if (result.getOrNull()?.on == true) open()
        }
    }
}

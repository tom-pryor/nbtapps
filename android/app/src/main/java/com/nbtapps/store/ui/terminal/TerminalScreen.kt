// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.terminal

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.view.WindowManager
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SmallFloatingActionButton
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.terminal.TerminalLine
import com.nbtapps.store.ui.rememberNotificationAsk
import com.nbtapps.store.ui.ssh.TurnOnSshDialog
import kotlinx.coroutines.launch

/** The shell's lines: a fixed-width face, as the cell the pty's size is worked out from. */
private val ShellText = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 13.sp, lineHeight = 18.sp)

/** The padding of the lines area, inside which the characters are counted. */
private val LinesPaddingH = 12.dp
private val LinesPaddingV = 8.dp

/** The texts of the states with no lines. */
internal const val TERMINAL_OPENING = "Opening a terminal…"
internal const val TERMINAL_HELD = "Paused while a job runs on the head unit"
internal const val TERMINAL_OFF = "SSH access is switched off"
internal const val TERMINAL_NO_PACKAGE = "SSH access isn't installed on this head unit"
internal const val TERMINAL_UNSUPPORTED = "This head unit's agent is too old for a terminal. Update the agent."
internal const val TERMINAL_ELSEWHERE = "A terminal is open on another head unit. The app has one open at a time."
internal const val TERMINAL_OTHER_CLOSED = "The terminal on the other head unit has closed."

/**
 * A shell on the head unit, through the SSH package: its output as lines, newest at the bottom; a row of control keys
 * and the user's earlier lines; a field with Send. Back leaves the shell open; Close ends it. See [TerminalViewModel].
 */
@Composable
fun TerminalScreen(
    onBack: () -> Unit,
    onOpenPackage: (packageId: String) -> Unit,
    viewModel: TerminalViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val density = LocalDensity.current
    val activity = LocalActivity.current

    // A shell's notification is how it stays in sight, and can be closed, once the app is left: the permission for it is
    // asked for as a shell is opened here, as the job screen asks as a job starts. The shell opens whatever the answer.
    val open = rememberNotificationAsk(viewModel::open)
    val confirmOn = rememberNotificationAsk(viewModel::confirmOn)
    val ask = rememberNotificationAsk {}
    var asked by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        // The screen came up opening one ([TerminalViewModel]'s start).
        if (!asked && uiState.view == TerminalView.Opening) {
            asked = true
            ask()
        }
    }

    // The keyboard reaches the layout only through its insets ([imePadding]): the window is not panned up to the field
    // as well, which with little room left pushed the lines and the field out of sight. As it was once the screen is left.
    DisposableEffect(activity) {
        val window = activity?.window
        val before = window?.attributes?.softInputMode
        if (window != null && before != null) {
            window.setSoftInputMode((before and WindowManager.LayoutParams.SOFT_INPUT_MASK_ADJUST.inv()) or WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        }
        onDispose { if (window != null && before != null) window.setSoftInputMode(restoredSoftInputMode(before)) }
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        // The room the screen has, the system's bars and the keyboard taken off: too short, and the layout is compact.
        val compact = with(density) {
            compactTerminal(
                heightDp = maxHeight.value,
                topDp = WindowInsets.systemBars.getTop(density).toDp().value,
                bottomDp = WindowInsets.systemBars.getBottom(density).toDp().value,
                imeDp = WindowInsets.ime.getBottom(density).toDp().value,
            )
        }
        TerminalLayout(uiState, viewModel, compact, onBack, onOpenPackage, context, open)
    }

    if (uiState.confirmingOn) TurnOnSshDialog(onConfirm = confirmOn, onDismiss = viewModel::cancelOn)
}

/**
 * The screen in its normal layout, or [compact]: with no top bar and no key row, the lines and the Command row with
 * Ctrl+C beside Send. System Back leaves it either way.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun TerminalLayout(
    uiState: TerminalUiState,
    viewModel: TerminalViewModel,
    compact: Boolean,
    onBack: () -> Unit,
    onOpenPackage: (packageId: String) -> Unit,
    context: Context,
    /** Opens a shell, from Open again, Try again and Open terminal: [TerminalViewModel.open] after the notification's ask. */
    onOpen: () -> Unit,
) {
    Scaffold(
        topBar = {
            if (!compact) TopAppBar(
                title = {
                    Column {
                        Text("Terminal", maxLines = 1)
                        uiState.name?.let {
                            Text(
                                it,
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back") }
                },
                actions = {
                    if (uiState.hasActions) {
                        val hasLines = uiState.lines.lines.isNotEmpty()
                        IconButton(
                            onClick = {
                                context.getSystemService(ClipboardManager::class.java)
                                    ?.setPrimaryClip(ClipData.newPlainText("NBTApps terminal", viewModel.copyText()))
                            },
                            enabled = hasLines,
                        ) { Icon(painterResource(R.drawable.ic_content_copy), contentDescription = "Copy all") }
                        IconButton(onClick = viewModel::clear, enabled = hasLines) {
                            Icon(painterResource(R.drawable.ic_clear_all), contentDescription = "Clear")
                        }
                        IconButton(onClick = viewModel::close, enabled = uiState.canClose) {
                            Icon(painterResource(R.drawable.ic_close), contentDescription = "Close")
                        }
                    }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .consumeWindowInsets(padding)
                .imePadding()
                .fillMaxSize(),
        ) {
            when (val view = uiState.view) {
                TerminalView.Opening, is TerminalView.Open, TerminalView.Ended -> Shell(uiState, viewModel, compact, onOpen)
                is TerminalView.Failed -> NoShell(view.reason, compact, error = true, button = "Try again", onClick = onOpen)
                TerminalView.Off -> NoShell(
                    TERMINAL_OFF,
                    compact,
                    button = "Switch on",
                    onClick = viewModel::askOn,
                    enabled = !uiState.switchingOn,
                    problem = uiState.switchError,
                )
                TerminalView.NoPackage -> NoShell(TERMINAL_NO_PACKAGE, compact, button = "Open in catalogue", onClick = { onOpenPackage(SshTunnel.PACKAGE) })
                TerminalView.Unsupported -> NoShell(TERMINAL_UNSUPPORTED, compact)
                TerminalView.Elsewhere -> NoShell(TERMINAL_ELSEWHERE, compact)
                TerminalView.OtherClosed -> NoShell(TERMINAL_OTHER_CLOSED, compact, button = "Open terminal", onClick = onOpen)
            }
        }
    }
}

/**
 * The lines, and below them the keys and the field, or Open again once the shell has ended. [compact]: no key row, and
 * Ctrl+C beside Send; Open again with less space around it.
 */
@Composable
private fun ColumnScope.Shell(uiState: TerminalUiState, viewModel: TerminalViewModel, compact: Boolean, onOpen: () -> Unit) {
    val view = uiState.view
    if (view == TerminalView.Open(held = true)) HeldBanner()
    Lines(uiState, viewModel, Modifier.weight(1f).fillMaxWidth())
    if (view == TerminalView.Ended) {
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        val vertical = if (compact) 4.dp else 12.dp
        Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = vertical, bottom = if (compact) 8.dp else 16.dp)) {
            Button(onClick = onOpen) { Text("Open again") }
        }
    } else {
        if (compact) {
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        } else {
            KeyRow(uiState.canType, viewModel)
        }
        InputRow(uiState.canType, viewModel, compact)
    }
}

@Composable
private fun HeldBanner() {
    Row(
        Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.tertiaryContainer)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(painterResource(R.drawable.ic_warning), contentDescription = null, tint = MaterialTheme.colorScheme.onTertiaryContainer)
        Text(TERMINAL_HELD, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onTertiaryContainer)
    }
}

/**
 * The lines, newest at the bottom. The view follows the newest line unless the user has scrolled up, when Jump to end
 * brings it back. The area's size in characters goes to the shell whenever it changes, the keyboard coming up too.
 */
@Composable
private fun Lines(uiState: TerminalUiState, viewModel: TerminalViewModel, modifier: Modifier) {
    val lines = uiState.lines.lines
    val opening = uiState.view == TerminalView.Opening
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()
    var following by rememberSaveable { mutableStateOf(true) }
    var areaHeight by remember { mutableIntStateOf(0) }

    val density = LocalDensity.current
    val measurer = rememberTextMeasurer()
    val cell = remember(measurer, density) {
        val sample = measurer.measure(CELL_SAMPLE, ShellText)
        sample.size.width / CELL_SAMPLE.length.toFloat() to sample.size.height.toFloat()
    }

    // A drag of the user's leaves the newest line; coming back to it, by a drag or a fling, follows again.
    LaunchedEffect(listState) {
        listState.interactionSource.interactions.collect { interaction ->
            when (interaction) {
                is DragInteraction.Start -> following = false
                is DragInteraction.Stop, is DragInteraction.Cancel -> if (!listState.canScrollForward) following = true
            }
        }
    }
    LaunchedEffect(listState) {
        snapshotFlow { listState.canScrollForward }.collect { more -> if (!more) following = true }
    }
    val count = lines.size + if (opening) 1 else 0
    LaunchedEffect(uiState.lines.revision, opening, areaHeight, following) {
        if (following && count > 0) listState.scrollToItem(count - 1, Int.MAX_VALUE)
    }

    Box(
        modifier
            .background(MaterialTheme.colorScheme.surfaceContainerLowest)
            .onSizeChanged { size ->
                areaHeight = size.height
                with(density) {
                    terminalGrid(
                        size.width - 2 * LinesPaddingH.roundToPx(),
                        size.height - 2 * LinesPaddingV.roundToPx(),
                        cell.first,
                        cell.second,
                    )
                }?.let(viewModel::sized)
            },
    ) {
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(horizontal = LinesPaddingH, vertical = LinesPaddingV),
            // Lines that do not fill the area sit at its foot, by the field, as a terminal's do; the opening line with
            // none above it at the top.
            verticalArrangement = if (opening && lines.isEmpty()) Arrangement.Top else Arrangement.Bottom,
        ) {
            itemsIndexed(lines) { _, line -> Line(line) }
            if (opening) {
                item {
                    Row(
                        Modifier.padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                        Text(TERMINAL_OPENING, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
        }
        if (!following && count > 0) {
            SmallFloatingActionButton(
                onClick = {
                    following = true
                    scope.launch { listState.scrollToItem(count - 1, Int.MAX_VALUE) }
                },
                modifier = Modifier.align(Alignment.BottomEnd).padding(end = 16.dp, bottom = 12.dp),
                containerColor = MaterialTheme.colorScheme.secondaryContainer,
                contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
            ) { Icon(painterResource(R.drawable.ic_arrow_downward), contentDescription = "Jump to end") }
        }
    }
}

/** One line: the shell's in the fixed-width face, wrapped; the app's own dimmed, in italics, in the ordinary face. */
@Composable
private fun Line(line: TerminalLine) {
    if (line.mark) {
        Text(
            line.text,
            modifier = Modifier.padding(vertical = 6.dp),
            style = MaterialTheme.typography.bodySmall.copy(fontStyle = FontStyle.Italic),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    } else {
        Text(line.text, style = ShellText, color = MaterialTheme.colorScheme.onSurface)
    }
}

/** Ctrl+C, Ctrl+D and Ctrl+Z at the left, the user's earlier and later lines at the right. */
@Composable
private fun KeyRow(enabled: Boolean, viewModel: TerminalViewModel) {
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Key("Ctrl+C", enabled) { viewModel.key(ControlKey.CTRL_C) }
        Key("Ctrl+D", enabled) { viewModel.key(ControlKey.CTRL_D) }
        Key("Ctrl+Z", enabled) { viewModel.key(ControlKey.CTRL_Z) }
        Spacer(Modifier.weight(1f))
        Row {
            IconButton(onClick = viewModel::earlier, enabled = enabled) {
                Icon(painterResource(R.drawable.ic_expand_less), contentDescription = "Earlier line")
            }
            IconButton(onClick = viewModel::later, enabled = enabled) {
                Icon(painterResource(R.drawable.ic_expand_more), contentDescription = "Later line")
            }
        }
    }
}

@Composable
private fun Key(label: String, enabled: Boolean, onClick: () -> Unit) {
    OutlinedButton(
        onClick = onClick,
        enabled = enabled,
        shape = RoundedCornerShape(8.dp),
        contentPadding = PaddingValues(horizontal = 12.dp),
        modifier = Modifier.height(40.dp),
    ) { Text(label) }
}

/**
 * The field and Send, and in the [compact] layout Ctrl+C beside them: stopping a command is the key most needed. Its
 * keyboard corrects nothing, capitalises nothing and suggests nothing.
 */
@Composable
private fun InputRow(enabled: Boolean, viewModel: TerminalViewModel, compact: Boolean) {
    Row(
        Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, top = 4.dp, bottom = if (compact) 4.dp else 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        OutlinedTextField(
            value = viewModel.field,
            onValueChange = viewModel::type,
            modifier = Modifier.weight(1f),
            enabled = enabled,
            singleLine = true,
            placeholder = { Text("Command") },
            keyboardOptions = KeyboardOptions(
                capitalization = KeyboardCapitalization.None,
                autoCorrectEnabled = false,
                keyboardType = KeyboardType.PasswordVisible,
                imeAction = ImeAction.Send,
            ),
            keyboardActions = KeyboardActions(onSend = { viewModel.send() }),
        )
        if (compact) Key("Ctrl+C", enabled) { viewModel.key(ControlKey.CTRL_C) }
        Button(onClick = viewModel::send, enabled = enabled) { Text("Send") }
    }
}

/**
 * A state with no shell: what it is, a line of what went wrong, and a button where there is something to do. Centred
 * where it fits, and scrolled where the room is too short for it; [compact] with less space around it.
 */
@Composable
private fun NoShell(
    text: String,
    compact: Boolean,
    error: Boolean = false,
    button: String? = null,
    onClick: () -> Unit = {},
    enabled: Boolean = true,
    problem: String? = null,
) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(if (compact) 16.dp else 32.dp),
            verticalArrangement = Arrangement.spacedBy(if (compact) 8.dp else 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text,
                style = MaterialTheme.typography.bodyLarge,
                color = if (error) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                textAlign = TextAlign.Center,
            )
            problem?.let {
                Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error, textAlign = TextAlign.Center)
            }
            button?.let { Button(onClick = onClick, enabled = enabled, modifier = Modifier.padding(top = 8.dp)) { Text(it) } }
        }
    }
}

/**
 * The soft input mode the window goes back to when the terminal is left, from the one it had, [before]. A window with no
 * adjust mode of its own, as the app's has, is panned to its field by Android; and "unspecified" cannot be set again,
 * as Android then keeps the mode there is, so pan is set as such.
 */
internal fun restoredSoftInputMode(before: Int): Int =
    if (before and WindowManager.LayoutParams.SOFT_INPUT_MASK_ADJUST == WindowManager.LayoutParams.SOFT_INPUT_ADJUST_UNSPECIFIED) {
        before or WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN
    } else {
        before
    }

/** What the width of one character is measured on: the mean of so many. */
private const val CELL_SAMPLE = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import androidx.annotation.DrawableRes
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.ui.headunits.AddHeadUnitFlow
import com.nbtapps.store.ui.headunits.AdvancedAutostart
import com.nbtapps.store.ui.headunits.AutostartChecklist
import com.nbtapps.store.ui.headunits.CONNECT_FROM_PHONE
import com.nbtapps.store.ui.headunits.CarPickerFlow
import com.nbtapps.store.ui.headunits.CheckFixDialog
import com.nbtapps.store.ui.headunits.ChecksLine
import com.nbtapps.store.ui.headunits.checksLine
import com.nbtapps.store.ui.setup.CardRow
import com.nbtapps.store.ui.setup.MutedText
import com.nbtapps.store.ui.setup.PageTitle
import com.nbtapps.store.ui.setup.SectionHeading
import com.nbtapps.store.ui.setup.StatusRow

/**
 * Pairs this phone with a car by a code another app handed over. Every page that still has the code shows it on a
 * card, with how long it is good for. The prompt asks which car it is for: a saved one, or one of the phone's paired
 * devices ([CarPickerFlow]), which Pair adds to the app; on Pair the screen connects and sends the code, showing
 * how that goes. The code cannot be changed here: one the agent refuses, or one that runs out, is replaced by a new
 * one from the app that sent it.
 *
 * With the box that has the car start the agent with it ticked, the screen stays once the phone is paired and shows
 * how setting that up goes ([AutostartChecklist]), until the user opens the app or closes it.
 *
 * [onPaired] is told once the agent has taken the code, where the box is cleared; [onOpenApp] when the user opens
 * the app from here, [onOpenHeadUnit] when the user opens the car's page in it, for what its checks found, and
 * [onClose] when the user is done with the screen.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PairRequestScreen(
    viewModel: PairRequestViewModel,
    onPaired: (String) -> Unit,
    onOpenApp: () -> Unit,
    onOpenHeadUnit: (String) -> Unit,
    onClose: () -> Unit,
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    var adding by rememberSaveable { mutableStateOf(false) }

    val step = uiState.step
    // The state is collected only while the screen is started, so the app is not opened from behind another.
    LaunchedEffect(step) {
        if (step is PairRequestStep.Paired && !uiState.enableAutostart) onPaired(step.name)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Pair this phone") },
                navigationIcon = {
                    IconButton(onClick = onClose) {
                        Icon(painterResource(R.drawable.ic_close), contentDescription = "Close")
                    }
                },
            )
        },
    ) { padding ->
        Box(Modifier.padding(padding).fillMaxSize()) {
            if (!uiState.loaded || uiState.code == null) {
                CircularProgressIndicator(Modifier.align(Alignment.Center))
                return@Box
            }
            val name = uiState.carName ?: "the car"
            when (step) {
                PairRequestStep.Prompt -> Prompt(
                    uiState = uiState,
                    onChoose = viewModel::choose,
                    onPickDifferent = viewModel::pickDifferent,
                    onPickDevice = viewModel::pickDevice,
                    onEnableAutostart = viewModel::setEnableAutostart,
                    onPair = viewModel::pair,
                    onAddByAddress = { adding = true },
                    onCancel = onClose,
                )
                PairRequestStep.Connecting, PairRequestStep.Pairing -> Sending(uiState, name, onCancel = onClose)
                is PairRequestStep.Paired -> Done(
                    uiState = uiState,
                    title = "Paired with ${step.name}",
                    text = PAIRED_TEXT,
                    name = step.name,
                    viewModel = viewModel,
                    onOpenApp = onOpenApp,
                    onOpenHeadUnit = onOpenHeadUnit,
                )
                is PairRequestStep.AlreadyPaired -> Done(
                    uiState = uiState,
                    title = "This phone is already paired with ${step.name}.",
                    text = "There was nothing to send the code for.",
                    name = step.name,
                    viewModel = viewModel,
                    onOpenApp = onOpenApp,
                    onOpenHeadUnit = onOpenHeadUnit,
                )
                PairRequestStep.TimeUp, is PairRequestStep.NoLink, is PairRequestStep.Refused -> Failed(
                    uiState = uiState,
                    name = uiState.carName,
                    onRetry = viewModel::pair,
                    onConnectFromPhone = viewModel::connectFromPhone,
                    onClose = onClose,
                    onOpenApp = onOpenApp,
                )
            }
        }
    }

    uiState.fix?.let { fix ->
        CheckFixDialog(fix, onConfirm = viewModel::confirmRemoval, onRetry = viewModel::retryRemoval, onClose = viewModel::closeRemoval)
    }

    if (uiState.takesAddress) {
        AddHeadUnitFlow(
            show = adding,
            onDismiss = { adding = false },
            onAdded = { id ->
                adding = false
                viewModel.added(id)
            },
        )
    }
}

/** A page of the screen: [content], which scrolls, and [buttons] at the bottom, which do not. */
@Composable
private fun Page(buttons: @Composable ColumnScope.() -> Unit, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Column(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
            content = content,
        )
        Column(
            Modifier.fillMaxWidth().padding(start = 24.dp, end = 24.dp, bottom = 16.dp, top = 8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            content = buttons,
        )
    }
}

/**
 * The code's card ([codeCard]): "Code" over the code, large and in monospace, and on the right "Good for" over the
 * countdown in a chip, where there is one. A code that has run out is struck through, with "Ran out" in its chip.
 */
@Composable
private fun CodeCardView(card: CodeCard, fullCode: String) {
    val colors = MaterialTheme.colorScheme
    Card(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Code", style = MaterialTheme.typography.labelLarge, color = colors.onSurfaceVariant)
                Text(
                    card.code,
                    // Read out digit by digit, and said to have run out where it has.
                    modifier = Modifier.semantics {
                        contentDescription = fullCode.toList().joinToString(" ") + if (card.ranOut) ", ran out" else ""
                    },
                    style = MaterialTheme.typography.headlineLarge.copy(
                        fontFamily = FontFamily.Monospace,
                        textDecoration = if (card.ranOut) TextDecoration.LineThrough else null,
                    ),
                    color = if (card.ranOut) colors.onSurfaceVariant else colors.onSurface,
                )
            }
            when {
                card.ranOut -> Chip("Ran out", colors.surfaceContainerHighest, colors.onSurfaceVariant)
                card.countdown != null -> Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Good for", style = MaterialTheme.typography.labelMedium, color = colors.onSurfaceVariant)
                    Chip(card.countdown, colors.tertiary, colors.onTertiary, monospace = true)
                }
            }
        }
    }
}

@Composable
private fun Chip(text: String, container: Color, content: Color, monospace: Boolean = false) {
    Surface(color = container, contentColor = content, shape = RoundedCornerShape(8.dp)) {
        Text(
            text,
            Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelLarge.copy(fontFamily = if (monospace) FontFamily.Monospace else null),
        )
    }
}

/** The code's card for [uiState], where the page has one. */
@Composable
private fun Code(uiState: PairRequestUiState) {
    val code = uiState.code ?: return
    codeCard(uiState)?.let { CodeCardView(it, code) }
}

/**
 * The prompt: the code, which car it goes to, the autostart box, and Pair. With head units saved they are offered
 * first, with "A different car…" that opens the picker; with none, the picker is there at once. In a build that
 * reaches a head unit at a network address, the add dialog is offered too.
 */
@Composable
private fun Prompt(
    uiState: PairRequestUiState,
    onChoose: (String) -> Unit,
    onPickDifferent: () -> Unit,
    onPickDevice: (PairedDevice, Boolean) -> Unit,
    onEnableAutostart: (Boolean) -> Unit,
    onPair: () -> Unit,
    onAddByAddress: () -> Unit,
    onCancel: () -> Unit,
) {
    val units = uiState.headUnits
    Page(
        buttons = {
            Button(onClick = onPair, Modifier.fillMaxWidth(), enabled = uiState.canPair) {
                Text(pairLabel(uiState.carName))
            }
            if (uiState.takesAddress) {
                TextButton(onClick = onAddByAddress, Modifier.fillMaxWidth()) { Text("Use a network address") }
            }
            TextButton(onClick = onCancel, Modifier.fillMaxWidth()) { Text("Cancel") }
        },
    ) {
        Code(uiState)
        MutedText(promptLead(anySaved = units.isNotEmpty()))
        PageTitle("Which car?")
        when {
            units.isEmpty() -> Unit
            // While a device is picked instead, the saved cars stay there to go back to.
            units.size == 1 && !uiState.picking -> SavedCar(units.single())
            else -> Card(Modifier.fillMaxWidth()) {
                Column(Modifier.selectableGroup()) {
                    units.forEachIndexed { index, unit ->
                        if (index > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                        SavedCarChoice(unit, selected = unit == uiState.chosen) { onChoose(unit.id) }
                    }
                }
            }
        }
        if (uiState.picking) {
            if (units.isNotEmpty()) SectionHeading("The cars paired with this phone")
            CarPickerFlow(
                selected = uiState.device?.address,
                onPick = { onPickDevice(it, false) },
                onPickDefault = { onPickDevice(it, true) },
                compact = true,
            )
        } else {
            TextButton(onClick = onPickDifferent) { Text("A different car…") }
        }
        if (uiState.device != null && uiState.newCar) {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                if (uiState.prePicked) MutedText(PICKED_FOR_YOU)
                MutedText(ADDED_ON_PAIR)
            }
        }
        AdvancedAutostart(uiState.enableAutostart, onEnableAutostart)
    }
}

/** The only saved car, named. */
@Composable
private fun SavedCar(unit: HeadUnit) {
    Card(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().heightIn(min = 56.dp).padding(horizontal = 16.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Icon(painterResource(R.drawable.ic_head_unit), contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(unit.name, style = MaterialTheme.typography.bodyLarge)
        }
    }
}

/** One of several saved cars, as a radio choice. */
@Composable
private fun SavedCarChoice(unit: HeadUnit, selected: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Icon(painterResource(R.drawable.ic_head_unit), contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(unit.name, Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
        RadioButton(selected = selected, onClick = null)
    }
}

/** Connecting and Pairing: the code, and how the link and the code go ([pairingRows]). */
@Composable
private fun Sending(uiState: PairRequestUiState, name: String, onCancel: () -> Unit) {
    Page(buttons = { TextButton(onClick = onCancel, Modifier.fillMaxWidth()) { Text("Cancel") } }) {
        Code(uiState)
        PageTitle("Pairing with $name")
        MutedText("Keep the phone in the car, with the ignition on.")
        Column {
            pairingRows(uiState.step, uiState.newCar, name).forEach { StatusRow(it.mark, it.text, note = it.note) }
        }
        Card(Modifier.fillMaxWidth()) {
            Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
                Icon(painterResource(R.drawable.ic_info), contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(STAY_HERE, style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

/**
 * Paired, now or from before: a tick by [title], [text], and with the box ticked the set-up to start the agent with
 * the car on a card, wired as on the car's page. "Open NBTApps" opens the app's screens.
 */
@Composable
private fun Done(
    uiState: PairRequestUiState,
    title: String,
    text: String,
    name: String,
    viewModel: PairRequestViewModel,
    onOpenApp: () -> Unit,
    onOpenHeadUnit: (String) -> Unit,
) {
    // With the box cleared the app's screens open at once: there is nothing to stay for.
    val stays = uiState.enableAutostart || uiState.step is PairRequestStep.AlreadyPaired
    Page(
        buttons = {
            if (stays) OutlinedButton(onClick = onOpenApp, Modifier.fillMaxWidth()) { Text("Open NBTApps") }
        },
    ) {
        TitleWithMark(R.drawable.ic_check, title, failed = false)
        MutedText(text)
        val setup = uiState.setupRun
        if (uiState.enableAutostart && setup != null) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Start with the car", Modifier.semantics { heading() }, style = MaterialTheme.typography.titleMedium)
                    AutostartChecklist(
                        setup = setup,
                        restartNeeded = uiState.restartNeeded,
                        onRetry = viewModel::retrySetup,
                        onRestart = viewModel::restartIdrive,
                        onRemove = viewModel::openRemoval,
                        onSkip = viewModel::skipRemoval,
                    )
                }
            }
            if (offersRestart(setup, uiState.restartNeeded)) {
                Text(RESTART_LOSES_LINKS, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            MutedText(GOES_ON)
        }
        val id = uiState.chosen?.id
        if (id != null && uiState.showsChecks) ChecksLine(checksLine(uiState.openChecks, name)) { onOpenHeadUnit(id) }
    }
}

/**
 * The code did not pair the phone ([failedPage]): it ran out, the agent refused it, or there was no link. The code,
 * a failed mark by the title, what happened, and where the car is saved that it stays. Retry where the code can still
 * go, else Close; and with Retry, where the connect only waited for the car's link and none came, the offer to
 * connect from the phone ([onConnectFromPhone]).
 */
@Composable
private fun Failed(
    uiState: PairRequestUiState,
    name: String?,
    onRetry: () -> Unit,
    onConnectFromPhone: () -> Unit,
    onClose: () -> Unit,
    onOpenApp: () -> Unit,
) {
    val page = failedPage(uiState, name) ?: return
    Page(
        buttons = {
            if (page.retry) {
                Button(onClick = onRetry, Modifier.fillMaxWidth()) { Text("Retry") }
                // Where the connect only waited for the car: the one way to have the phone try itself.
                if ((uiState.step as? PairRequestStep.NoLink)?.fromPhone == true) {
                    OutlinedButton(onClick = onConnectFromPhone, Modifier.fillMaxWidth()) { Text(CONNECT_FROM_PHONE) }
                }
                TextButton(onClick = onClose, Modifier.fillMaxWidth()) { Text("Close") }
            } else {
                Button(onClick = onClose, Modifier.fillMaxWidth()) { Text("Close") }
                TextButton(onClick = onOpenApp, Modifier.fillMaxWidth()) { Text("Open NBTApps") }
            }
        },
    ) {
        Code(uiState)
        TitleWithMark(R.drawable.ic_error, page.title, failed = true)
        if (page.error) {
            Text(page.message, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.error)
            if (!page.retry) MutedText(NEW_CODE)
        } else {
            MutedText(page.message)
        }
        val car = uiState.chosen
        if (car != null && carStays(uiState)) {
            Card(Modifier.fillMaxWidth()) {
                CardRow("${car.name} stays in NBTApps", "It is picked already when the next code comes.")
            }
        }
    }
}

/** A page's title with a large mark beside it: a tick, or, where [failed], the error sign. */
@Composable
private fun TitleWithMark(@DrawableRes icon: Int, title: String, failed: Boolean) {
    val colors = MaterialTheme.colorScheme
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        Box(
            Modifier.size(48.dp).background(if (failed) colors.errorContainer else colors.primaryContainer, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                painterResource(icon),
                contentDescription = null,
                Modifier.size(28.dp),
                tint = if (failed) colors.onErrorContainer else colors.onPrimaryContainer,
            )
        }
        PageTitle(title, Modifier.weight(1f))
    }
}

/** The code in two groups of three, as it is easier to read and compare: "123 456". */
internal fun grouped(code: String): String = code.chunked(3).joinToString(" ")

/** Whole seconds left, rounded up, as minutes and seconds: "1:05". */
internal fun countdown(remainingMs: Long): String {
    val seconds = (remainingMs + 999) / 1000
    return "${seconds / 60}:${(seconds % 60).toString().padStart(2, '0')}"
}

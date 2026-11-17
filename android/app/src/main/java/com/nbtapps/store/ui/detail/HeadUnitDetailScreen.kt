// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import android.Manifest
import android.text.format.DateUtils
import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.CheckProblem
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.RiskFlag
import com.nbtapps.store.ui.headunits.AutostartSetupCard
import com.nbtapps.store.ui.headunits.CheckFixDialog
import com.nbtapps.store.ui.headunits.Moves
import com.nbtapps.store.ui.headunits.Paths
import com.nbtapps.store.ui.headunits.movesHeading
import com.nbtapps.store.ui.headunits.removesHeading

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HeadUnitDetailScreen(
    onBack: () -> Unit,
    onUninstall: (packageId: String) -> Unit,
    onOpenJob: () -> Unit,
    onPair: (headUnitId: String) -> Unit,
    onOpenPackage: (packageId: String) -> Unit,
    onOpenLogs: (packageId: String) -> Unit,
    onOpenSettings: (packageId: String) -> Unit,
    viewModel: HeadUnitDetailViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        viewModel.refreshBluetooth()
    }
    // Pairing the car or switching Bluetooth on happens outside the app.
    LifecycleResumeEffect(Unit) {
        viewModel.refreshBluetooth()
        onPauseOrDispose {}
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(uiState.headUnit?.name ?: "Head unit") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        val unit = uiState.headUnit
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (unit == null) {
                if (uiState.loaded) Text("Head unit not found.") else CircularProgressIndicator()
                return@Column
            }
            val connected = uiState.link is LinkState.Connected
            ConnectionCard(
                unit,
                uiState,
                viewModel,
                onAllowBluetooth = { permissionLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT) },
                onPair = { onPair(unit.id) },
            )
            uiState.job?.let { JobCard(it, onOpenJob) }
            uiState.profile?.let {
                AgentCard(it, connected, uiState.restartError, viewModel::restart, onLogs = { onOpenLogs(AGENT_PACKAGE_ID) }.takeIf { uiState.logs })
            }
            // While the set-up runs, the problems it is about are its to ask.
            if (uiState.setup?.running != true) {
                ChecksCard(uiState, connected, viewModel::openFix, viewModel::dismissCheck, viewModel::recheck)
            }
            uiState.setup?.let { setup ->
                AutostartSetupCard(
                    step = setup.step,
                    restartNeeded = uiState.restartNeeded,
                    onRetry = viewModel::retrySetup,
                    onRestart = viewModel::restartForSetup,
                    onRemove = viewModel::openSetupRemoval,
                    onSkip = viewModel::skipSetupRemoval,
                    onOpenJob = onOpenJob,
                    onDismiss = viewModel::dismissSetup,
                )
            }
            uiState.autostart?.takeIf { connected }?.let { state ->
                // While the set-up runs, the switch is its to throw.
                val busy = uiState.autostartBusy || uiState.setup?.running == true
                AutostartCard(state, busy, uiState.autostartError, viewModel::setAutostart, onOpenPackage)
            }
            InstalledCard(uiState, connected, onUninstall, onOpenLogs, onOpenSettings)
            TroubleshootingCard(uiState, viewModel::resetSerial)
        }
    }

    uiState.fix?.let { step ->
        CheckFixDialog(step, onConfirm = viewModel::confirmFix, onRetry = viewModel::retryFix, onClose = viewModel::closeFix)
    }
}

/** Whether the checks' card has anything to show: problems that are not put aside, or a link to check again over. */
internal fun showsChecks(uiState: HeadUnitDetailUiState, connected: Boolean): Boolean =
    uiState.checks?.open?.isNotEmpty() == true || (connected && uiState.canCheck)

/** What the checks' card says where they found nothing, and where all they found was put aside. */
internal const val CHECKS_CLEAR = "Nothing put onto the head unit by hand is in the way."
internal const val CHECKS_ASIDE = "What the checks found is put aside. Check again shows it once more."

/**
 * What the card says, on the button that shows them, of the [moves] folders a problem's fix moves aside and the
 * [paths] paths it removes.
 */
internal fun listLabel(moves: Int, paths: Int, shown: Boolean): String {
    if (shown) return "Hide the list"
    val move = when (moves) {
        0 -> null
        1 -> "1 folder to move aside"
        else -> "$moves folders to move aside"
    }
    val remove = when (paths) {
        0 -> null
        1 -> "1 path to remove"
        else -> "$paths paths to remove"
    }
    return "Show the " + listOfNotNull(move, remove).joinToString(" and ")
}

/**
 * What the agent's checks found on the head unit and the user has not put aside: for each, the agent's own title and
 * detail, what it found, its fix where it has one, and Dismiss. A fix needs a link to this head unit; Check again
 * runs them all once more, and brings back what was put aside.
 */
@Composable
private fun ChecksCard(
    uiState: HeadUnitDetailUiState,
    connected: Boolean,
    onFix: (String) -> Unit,
    onDismiss: (String) -> Unit,
    onRecheck: () -> Unit,
) {
    if (!showsChecks(uiState, connected)) return
    val checks = uiState.checks
    val canFix = connected && uiState.canCheck
    Section("Checks") {
        when {
            checks == null -> Text(
                "The head unit hasn't been checked yet for what was put onto it by hand.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            checks.open.isEmpty() -> Text(
                if (checks.problems.isEmpty()) CHECKS_CLEAR else CHECKS_ASIDE,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        checks?.open?.forEach { problem -> ProblemRow(problem, canFix, onFix, onDismiss) }
        if (checks?.open?.any { it.fix != null } == true && !canFix) {
            Text(
                "Connect to the agent on this head unit to put these right.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (canFix) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onRecheck, enabled = !uiState.checking) { Text("Check again") }
                if (uiState.checking) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
            }
        }
        uiState.checkError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
    }
}

@Composable
private fun ProblemRow(problem: CheckProblem, canFix: Boolean, onFix: (String) -> Unit, onDismiss: (String) -> Unit) {
    var shown by rememberSaveable(problem.id) { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(problem.title, style = MaterialTheme.typography.titleSmall)
        Text(problem.detail, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (problem.paths.isNotEmpty() || problem.moves.isNotEmpty()) {
            TextButton(onClick = { shown = !shown }) { Text(listLabel(problem.moves.size, problem.paths.size, shown)) }
            if (shown) {
                if (problem.moves.isNotEmpty()) {
                    Text(movesHeading(problem.moves.size), style = MaterialTheme.typography.bodyMedium)
                    Moves(problem.moves)
                }
                if (problem.paths.isNotEmpty()) {
                    Text(removesHeading(problem.paths.size), style = MaterialTheme.typography.bodyMedium)
                    Paths(problem.paths)
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            problem.fix?.let { fix -> Button(onClick = { onFix(problem.id) }, enabled = canFix) { Text("${fix.label}…") } }
            TextButton(onClick = { onDismiss(problem.id) }) { Text("Dismiss") }
        }
    }
}

@Composable
private fun ConnectionCard(
    unit: HeadUnit,
    uiState: HeadUnitDetailUiState,
    viewModel: HeadUnitDetailViewModel,
    onAllowBluetooth: () -> Unit,
    onPair: () -> Unit,
) {
    Section("Connection") {
        unit.endpoints.forEach { Text(it.label) }
        uiState.serial?.let { SerialStatusLine(it, onAllowBluetooth) }
        when (val link = uiState.link) {
            LinkState.Unsupported -> Text(
                "This version of the app can't open a link to a head unit's agent yet. " +
                    "What is shown below is what the agent last reported, if it ever did.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            LinkState.Idle -> Button(onClick = viewModel::connect) { Text("Connect to the agent") }
            LinkState.Connecting -> Working("Connecting to the agent…")
            is LinkState.Connected -> {
                Text("Connected to the agent: ${link.over}", color = MaterialTheme.colorScheme.primary)
                OutlinedButton(onClick = viewModel::disconnect) { Text("Disconnect") }
            }
            is LinkState.NeedsPairing -> {
                Text(
                    if (link.stale) {
                        "The head unit no longer knows this phone: its paired phones were forgotten. Pair it again to go on."
                    } else {
                        "The agent answered, and wants this phone paired before it says more."
                    },
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onPair) { Text("Pair this phone") }
                    OutlinedButton(onClick = viewModel::disconnect) { Text("Disconnect") }
                }
            }
            is LinkState.Failed -> {
                Text(link.reason, color = MaterialTheme.colorScheme.error)
                Button(onClick = viewModel::connect) { Text("Connect again") }
            }
        }
    }
}

/** What the phone's Bluetooth says of the car, with no link opened. */
@Composable
private fun SerialStatusLine(status: SerialStatus, onAllow: () -> Unit) {
    if (status.problem == null) {
        Text(
            "Bluetooth is on and the phone is paired with the car.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    } else {
        Text("${status.problem}.", color = MaterialTheme.colorScheme.error)
        if (status.needsPermission) OutlinedButton(onClick = onAllow) { Text("Allow") }
    }
}

@Composable
private fun JobCard(view: JobView, onOpenJob: () -> Unit) {
    Section(if (view.ended) "Last job" else "Job in progress") {
        Text(
            when {
                view.forgotten -> "The head unit no longer has this job"
                view.linkLost -> "The link was lost while the job was running"
                else -> AgentText.state(view.job.state)
            },
        )
        TextButton(onClick = onOpenJob) { Text(if (view.ended) "Show result" else "Show progress") }
    }
}

@Composable
private fun AgentCard(profile: HeadUnitProfile, connected: Boolean, restartError: String?, onRestart: (String) -> Unit, onLogs: (() -> Unit)?) {
    val context = LocalContext.current
    Section("Agent") {
        Text("Version ${profile.agentVersion}")
        Text("Free for packages: ${Formatter.formatShortFileSize(context, profile.free.app)}")
        Text("Free for transfers: ${Formatter.formatShortFileSize(context, profile.free.staging)}")
        if (!connected) {
            Text(
                "As of ${DateUtils.getRelativeTimeSpanString(profile.seenAt)}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        AgentText.restart(profile.restartNeeded)?.let { need ->
            Text("$need what was installed.", color = MaterialTheme.colorScheme.tertiary)
        }
        if (connected) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { onRestart("hmi") }) { Text("Restart iDrive") }
                if (profile.restartNeeded == RestartKind.REBOOT) {
                    OutlinedButton(onClick = { onRestart("reboot") }) { Text("Reboot") }
                }
            }
        }
        onLogs?.let { TextButton(onClick = it) { Text("Get the agent's logs") } }
        restartError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
    }
}

/**
 * What the switch's card says under the switch: how things stand, and with nothing to do the starting, why.
 * Null where the switch says it all.
 */
internal fun autostartText(state: Autostart): String? = when {
    !state.available && state.on -> "It is switched on, but nothing starts the agent. " + AgentText.autostartReason(state.reason)
    !state.available -> AgentText.autostartReason(state.reason)
    state.on && state.starts >= AUTOSTART_STARTS_TO_SHOW ->
        "${state.starts} starts with the car are on record after which the agent didn't keep running. " +
            "After a few of these in a row the car stops trying."
    state.on -> "The iDrive plugin starts the agent each time the head unit starts."
    else -> "Off: after the head unit has restarted, the agent has to be started by hand before the app can reach it."
}

/**
 * From this many starts on record the card speaks of them. One is on record for the first minutes after every
 * start with the car, which is as it should be.
 */
internal const val AUTOSTART_STARTS_TO_SHOW = 2

/** Whether the agent is started with the car: the switch, and where it cannot be had, the package that brings it. */
@Composable
private fun AutostartCard(state: Autostart, busy: Boolean, error: String?, onSet: (Boolean) -> Unit, onOpenPackage: (String) -> Unit) {
    Section("Start with the car") {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Start the agent when the car starts", Modifier.weight(1f))
            // Off is always taken, so one that is on where nothing does the starting can still be switched off.
            Switch(checked = state.on, onCheckedChange = onSet, enabled = !busy && (state.available || state.on))
        }
        autostartText(state)?.let { text ->
            Text(
                text,
                style = MaterialTheme.typography.bodyMedium,
                color = if (state.available) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.tertiary,
            )
        }
        state.needs?.takeIf { !state.available }?.let { need ->
            OutlinedButton(onClick = { onOpenPackage(need.id) }) { Text("Show the iDrive plugin") }
        }
        if (state.available && state.on && state.starts >= AUTOSTART_STARTS_TO_SHOW) {
            // Switching on again is what forgets the starts on record.
            OutlinedButton(onClick = { onSet(true) }, enabled = !busy) { Text("Have the car try again") }
        }
        error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
    }
}

/** Whether a package's row offers its settings: there is a link to an agent that has them, and the package has some. */
internal fun showsSettings(uiState: HeadUnitDetailUiState, pkg: InstalledPackage): Boolean = uiState.settings && pkg.settings

@Composable
private fun InstalledCard(
    uiState: HeadUnitDetailUiState,
    connected: Boolean,
    onUninstall: (String) -> Unit,
    onOpenLogs: (String) -> Unit,
    onOpenSettings: (String) -> Unit,
) {
    Section("Installed packages") {
        when {
            uiState.profile == null && !connected -> Text("Not known yet: the app hasn't been connected to this head unit's agent.")
            uiState.installed.isEmpty() -> Text("Nothing is installed.")
            else -> uiState.installed.forEach { pkg ->
                InstalledRow(
                    pkg,
                    connected,
                    onUninstall,
                    onLogs = { onOpenLogs(pkg.id) }.takeIf { uiState.logs && pkg.logs },
                    onSettings = { onOpenSettings(pkg.id) }.takeIf { showsSettings(uiState, pkg) },
                )
            }
        }
        if (!connected && uiState.installed.isNotEmpty()) {
            Text(
                "Connect to the agent to uninstall a package.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun InstalledRow(
    pkg: InstalledPackage,
    connected: Boolean,
    onUninstall: (String) -> Unit,
    onLogs: (() -> Unit)?,
    onSettings: (() -> Unit)?,
) {
    val flags = pkg.flags.mapNotNull(RiskFlag::fromId).joinToString(" · ") { it.label }
    ListItem(
        headlineContent = { Text(pkg.name) },
        supportingContent = { Text(listOf("${pkg.id} · ${pkg.version}", flags).filter { it.isNotEmpty() }.joinToString("\n")) },
        trailingContent = {
            Column(horizontalAlignment = Alignment.End) {
                TextButton(onClick = { onUninstall(pkg.id) }, enabled = connected) { Text("Uninstall") }
                if (onLogs != null || onSettings != null) {
                    Row {
                        onSettings?.let { TextButton(onClick = it) { Text("Settings") } }
                        onLogs?.let { TextButton(onClick = it) { Text("Get logs") } }
                    }
                }
            }
        },
    )
}

/**
 * Whether the Troubleshooting card is shown: while there is a link to an agent that can have the car end this phone's
 * Bluetooth link, and no job needs the phone; and while a reset asked from this page runs or has an outcome to show,
 * since the reset itself ends the link.
 */
internal fun showsTroubleshooting(uiState: HeadUnitDetailUiState): Boolean = uiState.canResetSerial || uiState.serialReset != null

/** What the confirmation of the reset says it does, and when it is for. */
internal const val RESET_EXPLAINED =
    "The car ends this phone's Bluetooth link to the head unit, and the app connects again by itself. " +
        "This is for when the phone connects but nothing happens."

/** What can be done from the phone when the link to the head unit does not work as it should. */
@Composable
private fun TroubleshootingCard(uiState: HeadUnitDetailUiState, onReset: () -> Unit) {
    if (!showsTroubleshooting(uiState)) return
    var confirming by rememberSaveable { mutableStateOf(false) }
    val reset = uiState.serialReset
    Section("Troubleshooting") {
        ListItem(
            headlineContent = { Text("Reset Bluetooth link") },
            supportingContent = { Text("For when the phone connects but nothing happens.") },
            trailingContent = {
                if (reset == SerialResetUi.Working) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                } else {
                    TextButton(onClick = { confirming = true }, enabled = uiState.canResetSerial) { Text("Reset…") }
                }
            },
        )
        (reset as? SerialResetUi.Done)?.let { done ->
            Text(
                done.text,
                style = MaterialTheme.typography.bodyMedium,
                color = if (done.failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
    if (confirming) {
        AlertDialog(
            onDismissRequest = { confirming = false },
            title = { Text("Reset the Bluetooth link?") },
            text = { Text(RESET_EXPLAINED) },
            confirmButton = {
                TextButton(
                    onClick = {
                        confirming = false
                        onReset()
                    },
                ) { Text("Reset") }
            },
            dismissButton = { TextButton(onClick = { confirming = false }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun Working(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        Text(text)
    }
}

@Composable
private fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            content()
        }
    }
}

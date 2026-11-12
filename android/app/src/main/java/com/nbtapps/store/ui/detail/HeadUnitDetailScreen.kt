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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.RiskFlag

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HeadUnitDetailScreen(
    onBack: () -> Unit,
    onUninstall: (packageId: String) -> Unit,
    onOpenJob: () -> Unit,
    onPair: (headUnitId: String) -> Unit,
    onOpenPackage: (packageId: String) -> Unit,
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
            uiState.profile?.let { AgentCard(it, connected, uiState.restartError, viewModel::restart) }
            uiState.autostart?.takeIf { connected }?.let { state ->
                AutostartCard(state, uiState.autostartBusy, uiState.autostartError, viewModel::setAutostart, onOpenPackage)
            }
            InstalledCard(uiState, connected, onUninstall)
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
private fun AgentCard(profile: HeadUnitProfile, connected: Boolean, restartError: String?, onRestart: (String) -> Unit) {
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

@Composable
private fun InstalledCard(uiState: HeadUnitDetailUiState, connected: Boolean, onUninstall: (String) -> Unit) {
    Section("Installed packages") {
        when {
            uiState.profile == null && !connected -> Text("Not known yet: the app hasn't been connected to this head unit's agent.")
            uiState.installed.isEmpty() -> Text("Nothing is installed.")
            else -> uiState.installed.forEach { pkg -> InstalledRow(pkg, connected, onUninstall) }
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
private fun InstalledRow(pkg: InstalledPackage, connected: Boolean, onUninstall: (String) -> Unit) {
    val flags = pkg.flags.mapNotNull(RiskFlag::fromId).joinToString(" · ") { it.label }
    ListItem(
        headlineContent = { Text(pkg.name) },
        supportingContent = { Text(listOf("${pkg.id} · ${pkg.version}", flags).filter { it.isNotEmpty() }.joinToString("\n")) },
        trailingContent = {
            TextButton(onClick = { onUninstall(pkg.id) }, enabled = connected) { Text("Uninstall") }
        },
    )
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

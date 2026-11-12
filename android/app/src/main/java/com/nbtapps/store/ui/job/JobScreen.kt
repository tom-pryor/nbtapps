// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.job

import android.Manifest
import android.content.pm.PackageManager
import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.RestartKind

/** The words every plan and every running job carry: the head unit must stay powered until the job is done. */
internal const val IGNITION_NOTICE =
    "Keep the ignition on until this is done. If the head unit loses power while packages are being transferred, " +
        "the job has to be started again. If it loses power during the install, it undoes the unfinished step " +
        "when it next starts."

/** What stands over the packages that an update of the agent brought within reach. */
internal const val NOW_AVAILABLE = "The agent that ran before was too old for these. The new one runs them:"

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun JobScreen(
    onBack: () -> Unit,
    onInstall: (packageId: String, version: String) -> Unit,
    viewModel: JobViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current
    // The job's notification is how it stays visible once the screen is left. The job starts either way.
    val askToNotify = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { viewModel.start() }
    val start = {
        if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
            viewModel.start()
        } else {
            askToNotify.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }
    val job = uiState.job

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        when {
                            job != null -> AgentText.state(job.job.state)
                            uiState.removal -> "Uninstall"
                            uiState.review == Review.None -> "Job"
                            else -> "Install"
                        },
                    )
                },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (job != null) {
                JobContent(uiState, job, viewModel, onBack, onInstall)
            } else {
                ReviewContent(uiState, viewModel, onStart = start, onBack = onBack)
            }
            uiState.actionError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        }
    }
}

// ---- before the job: the plan ----

@Composable
private fun ColumnScope.ReviewContent(uiState: JobUiState, viewModel: JobViewModel, onStart: () -> Unit, onBack: () -> Unit) {
    when (val review = uiState.review) {
        Review.None -> Text("No job has run on a head unit since the app started.")
        Review.Planning -> Working("Asking the head unit for a plan…")
        is Review.Failed -> {
            Text(review.reason, color = MaterialTheme.colorScheme.error)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = viewModel::plan) { Text("Try again") }
                OutlinedButton(onClick = onBack) { Text("Back") }
            }
        }
        is Review.NeedsDownload -> {
            val context = LocalContext.current
            Section("Download first") {
                Text("These have to be on the phone before the head unit can be asked for a plan:")
                review.archives.forEach {
                    Text("• ${it.pkg.name} ${it.version.version} (${Formatter.formatShortFileSize(context, it.version.size)})")
                }
                review.error?.let { Text("The download failed: $it", color = MaterialTheme.colorScheme.error) }
                if (review.downloading) Working("Downloading…") else Button(onClick = viewModel::downloadMissing) { Text("Download") }
            }
        }
        is Review.Ready -> PlanContent(uiState, review.plan, viewModel, onStart, onBack)
    }
}

@Composable
private fun PlanContent(uiState: JobUiState, plan: Plan, viewModel: JobViewModel, onStart: () -> Unit, onBack: () -> Unit) {
    val context = LocalContext.current
    Section("What will happen") {
        if (plan.steps.isEmpty()) Text("Nothing.")
        plan.steps.forEachIndexed { index, step ->
            Text("${index + 1}. ${AgentText.step(step.action, uiState.name(step.id), step.version, step.oldVersion)}")
        }
        if (plan.fetchBytes > 0) Text("To transfer: ${Formatter.formatShortFileSize(context, plan.fetchBytes)}")
        if (plan.installBytes > 0) Text("On the head unit: ${Formatter.formatShortFileSize(context, plan.installBytes)}")
        AgentText.restart(plan.restart)?.let { Text("$it. The app offers it once the job is done.") }
    }
    if (uiState.removal) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(checked = uiState.deleteData, onCheckedChange = viewModel::setDeleteData)
            Text("Also delete the package's own data and settings")
        }
    }
    if (plan.riskFlags.isNotEmpty()) {
        Section("Before you start") {
            plan.riskFlags.forEach { flag ->
                Column {
                    Text(flag.label, style = MaterialTheme.typography.titleSmall)
                    Text(flag.explanation, style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
    }
    if (!plan.runnable) {
        Section("This plan can't run") {
            plan.missing.forEach { missing ->
                Text(
                    "${uiState.name(missing.requiredBy)} needs ${uiState.name(missing.id)}" +
                        (missing.version?.let { " ($it)" } ?: "") + ", which isn't on the phone or the head unit.",
                    color = MaterialTheme.colorScheme.error,
                )
            }
            plan.problems.forEach { problem ->
                Column {
                    Text(AgentText.problem(problem.code, uiState.name(problem.id)), color = MaterialTheme.colorScheme.error)
                    // The agent's own wording, for whoever has to work out why.
                    if (problem.message.isNotEmpty()) {
                        Text(problem.message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
    } else {
        IgnitionNotice()
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Button(onClick = onStart, enabled = plan.runnable && !uiState.starting) {
            Text(if (uiState.removal) "Uninstall" else "Start")
        }
        OutlinedButton(onClick = onBack) { Text("Cancel") }
    }
}

@Composable
private fun IgnitionNotice() {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Keep the ignition on", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onTertiaryContainer)
            Text(IGNITION_NOTICE, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onTertiaryContainer)
        }
    }
}

// ---- the job ----

@Composable
private fun JobContent(
    uiState: JobUiState,
    view: JobView,
    viewModel: JobViewModel,
    onBack: () -> Unit,
    onInstall: (packageId: String, version: String) -> Unit,
) {
    val job = view.job
    val result = job.result
    if (result == null) {
        Section("Step ${job.step} of ${job.steps}") {
            Text(uiState.name(job.id), style = MaterialTheme.typography.titleSmall)
            Text(if (job.state == JobState.PAUSED) AgentText.state(job.state) else AgentText.phase(job.phase))
            val total = job.total
            val bytes = job.bytes
            if (total != null && bytes != null && total > 0) {
                val context = LocalContext.current
                LinearProgressIndicator(progress = { bytes.toFloat() / total }, modifier = Modifier.fillMaxWidth())
                Text(
                    "${Formatter.formatShortFileSize(context, bytes)} of ${Formatter.formatShortFileSize(context, total)}",
                    style = MaterialTheme.typography.bodySmall,
                )
            } else if (!view.linkLost && !view.forgotten) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        }
        when {
            view.forgotten -> Section("The head unit no longer has this job") {
                Text("It lost power or its agent restarted before the install began. Nothing was changed. Plan it again.")
                OutlinedButton(onClick = onBack) { Text("Back") }
            }
            view.reconnecting -> {
                Section("Connecting again") {
                    Text(
                        "The head unit's agent ended the link to start its new version. " +
                            "The app connects again by itself and then shows how the update went.",
                    )
                    Working("Waiting for the agent…")
                }
                IgnitionNotice()
            }
            view.linkLost -> Section("The link was lost") {
                Text(
                    when (job.state) {
                        JobState.INSTALLING -> "The head unit finishes the install by itself. Connect again to see how it went."
                        JobState.RESTARTING -> "The head unit's agent was restarting to finish its own update. Connect again to see how it went."
                        else -> "The transfer waits on the head unit and goes on from where it stopped once the link is back."
                    },
                )
                Button(onClick = viewModel::reconnect) { Text("Connect again") }
            }
            else -> {
                IgnitionNotice()
                if (job.state == JobState.FETCHING || job.state == JobState.PAUSED) {
                    OutlinedButton(onClick = viewModel::stop, enabled = uiState.connected) { Text("Stop") }
                } else {
                    Text(
                        "The install has begun and runs to its end, with or without the phone.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    } else {
        Section(AgentText.state(result.state)) {
            result.steps.forEach { step ->
                Column {
                    Text(AgentText.step(step.action, uiState.name(step.id), step.version, step.oldVersion), style = MaterialTheme.typography.titleSmall)
                    Text(
                        AgentText.stepResult(step.result),
                        color = if (step.result == "done") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                    )
                    step.error?.let { error ->
                        Text(AgentText.problem(error.code, uiState.name(step.id)))
                        if (error.message.isNotEmpty()) {
                            Text(error.message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        }
        AgentText.restart(result.restart)?.let { need ->
            Section("Restart") {
                Text("$need what was installed.")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (result.restart == RestartKind.HMI) {
                        Button(onClick = { viewModel.restart("hmi") }, enabled = uiState.connected) { Text("Restart iDrive now") }
                    } else {
                        Button(onClick = { viewModel.restart("reboot") }, enabled = uiState.connected) { Text("Reboot now") }
                    }
                }
            }
        }
        if (uiState.nowAvailable.isNotEmpty()) {
            // The agent's own update went well. What needed it is one tap away: each opens its plan.
            Section("Now available") {
                Text(NOW_AVAILABLE)
                uiState.nowAvailable.forEach { item ->
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Column(Modifier.weight(1f)) {
                            Text(item.label, style = MaterialTheme.typography.titleSmall)
                            Text(
                                item.installed?.let { "The head unit has $it" } ?: "Not installed",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        OutlinedButton(onClick = { onInstall(item.id, item.version) }) {
                            Text(if (item.installed != null) "Update" else "Install")
                        }
                    }
                }
            }
        }
        Button(onClick = onBack) { Text("Done") }
    }
    if (view.log.isNotEmpty()) {
        Section("Log") {
            Text(
                view.log.takeLast(200).joinToString("") { line ->
                    val prefix = if (line.source == "agent") "" else "${line.source}: "
                    line.text.trimEnd('\n').lines().joinToString("") { "$prefix$it\n" }
                }.trimEnd('\n'),
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.bodySmall,
            )
        }
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

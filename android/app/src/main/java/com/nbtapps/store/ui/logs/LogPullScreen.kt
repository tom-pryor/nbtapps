// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.logs

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.text.format.DateUtils
import android.text.format.Formatter
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.logs.LogPullState
import com.nbtapps.store.model.AgentFile
import java.io.File

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogPullScreen(onBack: () -> Unit, viewModel: LogPullViewModel = hiltViewModel()) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current
    // The ZIP of a pull started here goes to the share sheet as soon as it is made.
    LaunchedEffect(uiState.shareNow) {
        uiState.shareNow?.let { zip ->
            viewModel.shared(zip)
            share(context, zip)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Logs of ${uiState.name}") },
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
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                "The files go into a ZIP on this phone, which you can then send to whoever asked for them. " +
                    "Look through them first if you like: a log may say more about the car than you want to pass on.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (!uiState.available && uiState.pull !is LogPullState.Done) {
                Text("Connect to the head unit to get its logs. Its agent has to be one that can send them.")
            }
            when (val listing = uiState.listing) {
                Listing.Loading -> if (uiState.available) CircularProgressIndicator()
                is Listing.Failed -> {
                    Text(listing.reason, color = MaterialTheme.colorScheme.error)
                    OutlinedButton(onClick = viewModel::reload) { Text("Try again") }
                }
                is Listing.Ready -> Files(uiState, listing, viewModel)
            }
            Pull(uiState, viewModel, onShare = { zip -> share(context, zip) })
        }
    }
}

@Composable
private fun Files(uiState: LogPullUiState, listing: Listing.Ready, viewModel: LogPullViewModel) {
    val context = LocalContext.current
    if (listing.files.isEmpty()) {
        Text("There are no log files of this package on the head unit.")
        return
    }
    listing.files.forEach { file -> FileRow(file, file.path in uiState.chosen, enabled = !uiState.pulling) { viewModel.choose(file.path, it) } }
    if (listing.more) {
        Text(
            "The head unit has older files too, which it couldn't list in one answer.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        // In KB of 1,024 bytes, as the constant is: Android's formatter would call it 262 kB.
        Text("Only the last ${LogPullViewModel.TAIL_BYTES / 1024} KB of each file", Modifier.weight(1f))
        Switch(checked = uiState.tail, onCheckedChange = viewModel::setTail, enabled = !uiState.pulling)
    }
    if (uiState.chosen.isNotEmpty()) {
        Text(
            "${Formatter.formatShortFileSize(context, uiState.bytes)} in all. Estimate over Bluetooth: up to ${duration(uiState.estimateSeconds)}; " +
                "logs mostly compress to much less.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    Button(onClick = viewModel::start, enabled = uiState.canPull) { Text("Get logs") }
}

@Composable
private fun FileRow(file: AgentFile, chosen: Boolean, enabled: Boolean, onChoose: (Boolean) -> Unit) {
    val context = LocalContext.current
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
        Checkbox(checked = chosen, onCheckedChange = onChoose, enabled = enabled)
        Column(Modifier.weight(1f)) {
            Text(file.path, style = MaterialTheme.typography.bodyMedium)
            val changed = if (file.modified > 0) {
                " · changed " + DateUtils.formatDateTime(context, file.modified * 1000, DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_TIME)
            } else {
                ""
            }
            Text(
                Formatter.formatShortFileSize(context, file.size) + changed,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun Pull(uiState: LogPullUiState, viewModel: LogPullViewModel, onShare: (File) -> Unit) {
    when (val pull = uiState.pull) {
        null -> Unit
        is LogPullState.Pulling -> {
            Text("Getting file ${pull.file} of ${pull.files}…")
            if (pull.total > 0) {
                LinearProgressIndicator(progress = { (pull.bytes.toFloat() / pull.total).coerceIn(0f, 1f) }, modifier = Modifier.fillMaxWidth())
            } else {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
            OutlinedButton(onClick = viewModel::cancel) { Text("Cancel") }
        }
        is LogPullState.Done -> {
            val zip = uiState.zip
            Text(
                when (pull.included) {
                    pull.files.size -> "Got ${files(pull.included)}."
                    else -> "Got ${files(pull.included)} of ${pull.files.size}. The ZIP's info.txt says why the others are missing."
                },
            )
            pull.files.filter { it.failure != null }.forEach { Text("${it.file.path}: ${it.failure}", color = MaterialTheme.colorScheme.error) }
            if (zip != null) {
                Button(onClick = { onShare(zip) }) { Text("Share again") }
            } else {
                Text("The ZIP is gone from the phone. Get the logs again to share them.")
            }
        }
        is LogPullState.Failed -> Text(pull.reason, color = MaterialTheme.colorScheme.error)
        is LogPullState.Cancelled -> Text("Stopped. Nothing was kept.")
    }
}

private fun files(count: Int) = if (count == 1) "1 file" else "$count files"

/** At most how long: in seconds below two minutes, and in minutes from there. */
internal fun duration(seconds: Long): String = when {
    seconds < 120 -> "about $seconds seconds"
    else -> "about ${(seconds + 59) / 60} minutes"
}

/** Hands [zip] to Android's share sheet, readable by whichever app the user sends it with. */
private fun share(context: Context, zip: File) {
    val uri = FileProvider.getUriForFile(context, "${context.packageName}.share", zip)
    val send = Intent(Intent.ACTION_SEND)
        .setType("application/zip")
        .putExtra(Intent.EXTRA_STREAM, uri)
        .putExtra(Intent.EXTRA_SUBJECT, zip.name)
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    // The grant goes with the clip: the chooser passes it on to the app the user picks.
    send.clipData = ClipData.newRawUri(zip.name, uri)
    context.startActivity(Intent.createChooser(send, "Send the logs"))
}

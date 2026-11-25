// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R
import com.nbtapps.store.data.whatsnew.VersionNotes
import java.time.LocalDate
import java.time.format.DateTimeFormatter
import java.time.format.DateTimeParseException
import java.util.Locale
import kotlinx.coroutines.launch

/** The title over the notes of a version: "What's new in 0.9.0". */
internal fun whatsNewTitle(notes: VersionNotes): String = "What's new in ${notes.versionName}"

/**
 * The line under the title: the app and the version, with the release date where the catalogue gives one:
 * "NBTApps 0.9.0 · 23 November 2026". The date is written out as [longReleaseDate] writes it.
 */
internal fun whatsNewByline(appName: String, notes: VersionNotes): String =
    listOfNotNull("$appName ${notes.versionName}", notes.released?.takeIf { it.isNotBlank() }?.let(::longReleaseDate))
        .joinToString(" · ")

/** Day, month name and year, in English whatever the phone's language, as the app is in English only. */
private val LONG_DATE: DateTimeFormatter = DateTimeFormatter.ofPattern("d MMMM uuuu", Locale.ENGLISH)

/**
 * A release date of the catalogue in long form: a plain date such as "2026-11-23" as "23 November 2026", and
 * "2026-11-05" as "5 November 2026"; anything else as it comes.
 */
internal fun longReleaseDate(released: String): String =
    try {
        LocalDate.parse(released, DateTimeFormatter.ISO_LOCAL_DATE).format(LONG_DATE)
    } catch (e: DateTimeParseException) {
        released
    }

/**
 * The notes the sheet shows now: [notes], those that are due, while the catalogue is on top ([onTop]) and the side menu
 * is not over it ([menuShown]). Otherwise they wait, and stay due.
 */
internal fun notesOverCatalogue(notes: VersionNotes?, onTop: Boolean, menuShown: Boolean): VersionNotes? =
    notes.takeIf { onTop && !menuShown }

/**
 * What is new in the version that runs, over the catalogue, the first time it is opened after an update. The notes
 * scroll within the sheet, with OK always in sight under them. However the sheet goes, by OK, a swipe down, a tap
 * beside it or Back, [onDismiss] is told once it has gone, and the notes count as seen.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WhatsNewSheet(notes: VersionNotes, onDismiss: () -> Unit) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scope = rememberCoroutineScope()
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(
            Modifier.fillMaxWidth().padding(start = 24.dp, end = 24.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            WhatsNewHeading(notes)
            // Takes what room is left, and no more: a short list leaves the button just under it.
            Column(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState())) {
                WhatsNewLines(notes.lines)
            }
            Button(
                onClick = { scope.launch { sheetState.hide() }.invokeOnCompletion { onDismiss() } },
                modifier = Modifier.fillMaxWidth(),
            ) { Text("OK") }
        }
    }
}

/** The title and the line under it, as the sheet and the page of what is new show them. */
@Composable
internal fun WhatsNewHeading(notes: VersionNotes, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(whatsNewTitle(notes), Modifier.semantics { heading() }, style = MaterialTheme.typography.headlineSmall)
        Text(
            whatsNewByline(stringResource(R.string.app_name), notes),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.bodyMedium,
        )
    }
}

/**
 * The notes as a list, a bullet each, with a line that wraps kept clear of the bullets. Bullet and note are of one
 * style, the size of the line under the title, so that the bullet sits on the note's first line.
 */
@Composable
internal fun WhatsNewLines(lines: List<String>, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        lines.forEach { line ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("•", style = MaterialTheme.typography.bodyMedium)
                Text(line, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

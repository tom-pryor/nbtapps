// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.appsettings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.ui.catalogue.WhatsNewHeading
import com.nbtapps.store.ui.catalogue.WhatsNewLines
import com.nbtapps.store.ui.setup.openWebPage

/**
 * What the page says where the phone has no notes of the version that runs, a case that is not expected: the row in
 * the app's settings opens this page only where the phone has them, and opens the changelog itself otherwise. Should
 * the notes go missing between the row and the page, the page says this.
 */
internal fun noNotesText(version: String): String = "The notes of version $version aren't on this phone yet."

/**
 * What is new in the version that runs, as a page opened from the app's settings: the notes the sheet over the
 * catalogue shows once after an update. Leaving it marks nothing as seen: the sheet is the sheet's business.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WhatsNewScreen(onBack: () -> Unit, viewModel: AppSettingsViewModel = hiltViewModel()) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current
    Scaffold(
        topBar = { TopAppBar(title = {}, navigationIcon = { BackButton(onBack) }) },
    ) { padding ->
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            val state = uiState ?: return@Column
            val notes = state.notes
            if (notes == null) {
                Text(noNotesText(state.version), style = MaterialTheme.typography.bodyLarge)
                TextButton(onClick = { openWebPage(context, CHANGELOG_URL) }) { Text("Open the changelog on GitHub") }
                return@Column
            }
            WhatsNewHeading(notes, Modifier.padding(horizontal = 4.dp))
            Card(Modifier.fillMaxWidth().padding(bottom = 16.dp)) {
                WhatsNewLines(notes.lines, Modifier.padding(16.dp))
            }
        }
    }
}

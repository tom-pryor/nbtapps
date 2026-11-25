// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.about

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.ui.setup.openWebPage

/** The gear in a top bar, which opens the app's About page. */
@Composable
fun AboutButton(onClick: () -> Unit) {
    IconButton(onClick = onClick) {
        Icon(painterResource(R.drawable.ic_settings), contentDescription = "About")
    }
}

/**
 * About the app: its version and what is new in it ([onOpenWhatsNew]), the licences of the code it includes
 * ([onOpenLicences]), and its page on GitHub.
 */
@Composable
fun AboutScreen(
    onBack: () -> Unit,
    onOpenWhatsNew: () -> Unit,
    onOpenLicences: () -> Unit,
    viewModel: AboutViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current
    AboutContent(uiState, onBack) { target ->
        when (target) {
            AboutTarget.WhatsNew -> onOpenWhatsNew()
            AboutTarget.Licences -> onOpenLicences()
            is AboutTarget.WebPage -> openWebPage(context, target.url)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AboutContent(uiState: AboutUiState?, onBack: () -> Unit, onOpen: (AboutTarget) -> Unit) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("About") },
                navigationIcon = { BackButton(onBack) },
            )
        },
    ) { padding ->
        Column(
            Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (uiState == null) return@Column
            RowGroup("About NBTApps", aboutRows(uiState), onOpen)
            RowGroup("More", moreRows, onOpen, Modifier.padding(top = 16.dp))
        }
    }
}

/** The back arrow of the About page, and of the pages opened from it. */
@Composable
internal fun BackButton(onBack: () -> Unit) {
    IconButton(onClick = onBack) {
        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back")
    }
}

/** A heading, and its rows on one card. */
@Composable
private fun RowGroup(title: String, rows: List<AboutRow>, onOpen: (AboutTarget) -> Unit, modifier: Modifier = Modifier) {
    Text(
        title,
        modifier.padding(horizontal = 4.dp).semantics { heading() },
        color = MaterialTheme.colorScheme.primary,
        style = MaterialTheme.typography.titleSmall,
    )
    Card(Modifier.fillMaxWidth()) {
        rows.forEachIndexed { i, row ->
            if (i > 0) HorizontalDivider(Modifier.padding(horizontal = 16.dp))
            RowItem(row, onOpen)
        }
    }
}

/** A row: a chevron where it opens a page of the app, an arrow out where it opens the browser, nothing where it only says. */
@Composable
private fun RowItem(row: AboutRow, onOpen: (AboutTarget) -> Unit) {
    val target = row.target
    ListItem(
        modifier = if (target != null) Modifier.clickable { onOpen(target) } else Modifier,
        headlineContent = { Text(row.title) },
        supportingContent = { Text(row.detail) },
        trailingContent = target?.let {
            {
                Icon(
                    painterResource(if (it is AboutTarget.WebPage) R.drawable.ic_open_in_new else R.drawable.ic_chevron_right),
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
    )
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.ssh

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.ssh.SshTunnel

/**
 * The SSH page of a head unit, opened from the side menu and from the head unit's page: SSH access switched on and off,
 * the way to the terminal and to SSH from other apps ([onOpenTerminal], [onOpenSshAccess]), or the SSH package's page
 * where it is not installed ([onOpenPackage]). See [SshViewModel].
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SshScreen(
    onBack: () -> Unit,
    onOpenTerminal: (headUnitId: String) -> Unit,
    onOpenSshAccess: (headUnitId: String) -> Unit,
    onOpenPackage: (packageId: String) -> Unit,
    viewModel: SshViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("SSH", maxLines = 1, overflow = TextOverflow.Ellipsis)
                        uiState.name?.let {
                            Text(it, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                        }
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back") }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (uiState.apart) ApartBanner(connecting = uiState.connecting, onConnect = viewModel::connect)
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                when (val content = uiState.content) {
                    SshPageContent.Loading -> CircularProgressIndicator()
                    SshPageContent.NotFound -> Text("Head unit not found.")
                    SshPageContent.Install -> PageCard { SshInstallRow(onInstall = { onOpenPackage(SshTunnel.PACKAGE) }) }
                    SshPageContent.CantRun -> PageCard {
                        ListItem(headlineContent = { Text(SSH_ACCESS) }, supportingContent = { Text(SSH_CANT_RUN) })
                    }
                    is SshPageContent.Controls -> PageCard {
                        SshCardRows(
                            content.card,
                            onSwitch = viewModel::switch,
                            onOpenTerminal = { onOpenTerminal(viewModel.headUnitId) },
                            onOpenPort = { onOpenSshAccess(viewModel.headUnitId) },
                        )
                    }
                }
            }
        }
    }

    if (uiState.confirming) TurnOnSshDialog(onConfirm = viewModel::confirm, onDismiss = viewModel::cancel)
}

/** The page's one card, with no title of its own: the top bar names it. */
@Composable
private fun PageCard(content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) { content() }
    }
}

/** The link is not to this head unit: in the look of the terminal's banner while a job holds it, with Connect. */
@Composable
private fun ApartBanner(connecting: Boolean, onConnect: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.tertiaryContainer).padding(start = 16.dp, end = 8.dp, top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(painterResource(R.drawable.ic_warning), contentDescription = null, tint = MaterialTheme.colorScheme.onTertiaryContainer)
        Text(
            SSH_APART,
            Modifier.weight(1f).padding(vertical = 8.dp),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onTertiaryContainer,
        )
        TextButton(
            onClick = onConnect,
            enabled = !connecting,
            colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.onTertiaryContainer),
        ) { Text("Connect") }
    }
}

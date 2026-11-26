// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import com.nbtapps.store.ui.ssh.SshInstallRow

/**
 * The Debug card on a head unit's page: one row, SSH, which opens the SSH page. Shown where the agent has channels and
 * the SSH package is installed, or the catalogue offers it for the head unit; with no link, where the package was
 * installed when the agent last said.
 */
sealed interface DebugCard {
    /** The package is not on the head unit, and the catalogue offers it: the row offers its install. */
    data object Install : DebugCard

    /** The package was there when the agent last said, and there is no link to the head unit: the row is greyed. */
    data object Apart : DebugCard

    /** The package is there, over the link: [line] says how SSH stands, as the side menu does; none until it is read. */
    data class Ready(val line: String?) : DebugCard

    /** The row's line under SSH. */
    val rowLine: String? get() = when (this) {
        Install -> null
        Apart -> SSH_CONNECT_FIRST
        is Ready -> line
    }

    /** Open may be pressed: over the link only. */
    val canOpen: Boolean get() = this is Ready
}

/** The row's line with no link to the head unit. */
internal const val SSH_CONNECT_FIRST = "Connect to the head unit to use SSH"

/** The card: the install's row where the package is not there; otherwise SSH, how it stands, and Open. */
@Composable
internal fun DebugCardSection(card: DebugCard, onInstall: () -> Unit, onOpen: () -> Unit) {
    Section("Debug") {
        if (card == DebugCard.Install) {
            SshInstallRow(onInstall)
            return@Section
        }
        val greyed = !card.canOpen
        val colors = if (greyed) {
            ListItemDefaults.colors(headlineColor = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            ListItemDefaults.colors()
        }
        ListItem(
            headlineContent = { Text("SSH") },
            supportingContent = card.rowLine?.let { { Text(it) } },
            trailingContent = { TextButton(onClick = onOpen, enabled = card.canOpen) { Text("Open") } },
            colors = colors,
        )
    }
}

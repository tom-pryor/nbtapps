// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.ssh

import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.service.connectionsText

/**
 * The card of the SSH page where the SSH package is on the head unit: SSH access, the SSH package's switch; the
 * terminal; and SSH from other apps, the port on the phone.
 */
data class SshCard(
    /**
     * Whether SSH access is switched on, as the agent said last over the link that is open to this head unit. Null
     * where that is not known: no link, or no answer yet. The switch is then shown off, and greyed.
     */
    val on: Boolean? = null,
    /**
     * How many SSH connections are open on the head unit, on every link, from the same answer. Null where the agent's
     * answer gave none. See [com.nbtapps.store.data.ssh.SshSwitchState.connections].
     */
    val connections: Int? = null,
    /** The switch has been thrown, and the agent has not answered yet. */
    val switching: Boolean = false,
    /** Why the agent did not take the last throw of the switch, in a line for the user. */
    val error: String? = null,
    /** A terminal is open, or being opened, on this head unit: the second row goes back to it. */
    val terminalOpen: Boolean = false,
    /** The SSH port for other apps, as it stands for this head unit: off where it is another's. */
    val port: SshPortState = SshPortState.Off(),
) {
    /** The third row's line: what the port is for, why it cannot be set up yet, or how it stands. */
    val portLine: String
        get() = when (val now = port) {
            is SshPortState.On -> "${if (now.otherDevices) "On for other devices" else "On"} · port ${now.port} · ${connectionsText(now.connections)}"
            is SshPortState.Checking -> "Checking the way to the head unit…"
            is SshPortState.Off -> if (on == true) PORT_OFFER else SSH_FIRST
        }

    /** The third row's button: Manage while the port is on, or being switched on, here; Set up otherwise. */
    val portButton: String get() = if (port.active) "Manage" else "Set up"

    /** The third row's button may be pressed: always to manage the port; to set it up, with SSH access on. */
    val canOpenPort: Boolean get() = port.active || on == true

    /** The switch may be thrown: how it stands is known, and nothing waits for the agent. */
    val canSwitch: Boolean get() = on != null && !switching

    /** The second row's button may be pressed: to go back to the terminal that is open, or to open one. */
    val canOpenTerminal: Boolean get() = terminalOpen || on == true

    /** What the switch's row says under its name: how many connections are open, or that the switch does not last. */
    val switchLine: String get() = connectionsOpen(connections) ?: SSH_OFF_AFTER_RESTART
}

/** "1 connection open", "2 connections open"; null for none, and where the count is not known. */
internal fun connectionsOpen(count: Int?): String? = when {
    count == null || count <= 0 -> null
    count == 1 -> "1 connection open"
    else -> "$count connections open"
}

internal const val SSH_ACCESS = "SSH access"
internal const val SSH_OFFER = "A root shell on the head unit, for debugging"
internal const val SSH_OFF_AFTER_RESTART = "Off again after the head unit restarts"
internal const val PORT_OFFER = "For an SSH app on this phone, or a laptop"
internal const val SSH_FIRST = "Switch on SSH access first"
internal const val SSH_TURN_ON_TITLE = "Turn on SSH access?"
internal const val SSH_TURN_ON_TEXT = "This gives this phone a root shell on the head unit. A wrong command can stop it working."

/** The row that offers the SSH package's install, where it is not on the head unit: on the SSH page and the head unit's. */
@Composable
internal fun SshInstallRow(onInstall: () -> Unit) {
    ListItem(
        headlineContent = { Text(SSH_ACCESS) },
        supportingContent = { Text(SSH_OFFER) },
        trailingContent = { TextButton(onClick = onInstall) { Text("Install…") } },
    )
}

/** The card's rows: the switch, a line where the agent refused it, the terminal's row and the port's. */
@Composable
internal fun SshCardRows(
    card: SshCard,
    onSwitch: (Boolean) -> Unit,
    onOpenTerminal: () -> Unit,
    onOpenPort: () -> Unit,
) {
    ListItem(
        headlineContent = { Text(SSH_ACCESS) },
        supportingContent = { Text(card.switchLine) },
        trailingContent = { Switch(checked = card.on == true, onCheckedChange = onSwitch, enabled = card.canSwitch) },
    )
    card.error?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error) }
    ListItem(
        headlineContent = { Text(if (card.terminalOpen) "Terminal open" else "Open terminal") },
        trailingContent = {
            TextButton(onClick = onOpenTerminal, enabled = card.canOpenTerminal) { Text(if (card.terminalOpen) "Resume" else "Open") }
        },
    )
    ListItem(
        headlineContent = { Text("SSH from other apps") },
        supportingContent = { Text(card.portLine) },
        trailingContent = { TextButton(onClick = onOpenPort, enabled = card.canOpenPort) { Text(card.portButton) } },
    )
}

/** The question before SSH access is switched on, from the SSH page and from the terminal. Switching off asks nothing. */
@Composable
fun TurnOnSshDialog(onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(SSH_TURN_ON_TITLE) },
        text = { Text(SSH_TURN_ON_TEXT) },
        confirmButton = { TextButton(onClick = onConfirm) { Text("Turn on") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

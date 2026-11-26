// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.terminal.TerminalState

/**
 * Where SSH stands on the head unit the app is connected to, as the side menu's entry shows it: see [sshEntry]. Each
 * state but [NotConnected] is of the head unit [headUnitId], whose name is [name].
 */
sealed interface SshEntry {
    /** No link is up: none, one being made, or one waiting for the phone to be paired. */
    data object NotConnected : SshEntry

    /** The SSH package is not on the head unit, and the catalogue offers it, or has not been read yet. */
    data class NotInstalled(val headUnitId: String, val name: String) : SshEntry

    /** The head unit's agent has no channels, or the package is not there and the catalogue offers none that runs on it. */
    data class CantRun(val headUnitId: String, val name: String) : SshEntry

    /**
     * The package is there, and its agent has channels. [on] is the switch as the agent last said, null until it has;
     * [terminalOpen] a terminal is open or being opened on it; [port] the SSH port for other apps as it stands for it,
     * off where it is another's.
     */
    data class Ready(
        val headUnitId: String,
        val name: String,
        val on: Boolean? = null,
        val terminalOpen: Boolean = false,
        val port: SshPortState = SshPortState.Off(),
    ) : SshEntry {
        /**
         * The line under SSH, made of what is open: "Off", "On", "On · terminal open · port 2222". None while the switch
         * is not known.
         */
        val line: String?
            get() {
                val on = on ?: return null
                return buildList {
                    add(if (on) "On" else "Off")
                    if (terminalOpen) add("terminal open")
                    (port as? SshPortState.On)?.let { add("port ${it.port}") }
                }.joinToString(" · ")
            }
    }
}

/** What a head unit is called where the saved head units do not name it. */
internal const val UNNAMED = "Head unit"

/**
 * The menu's entry for SSH: [NotConnected][SshEntry.NotConnected] with no link up; with one, [CantRun][SshEntry.CantRun]
 * where the agent has no channels; [Ready][SshEntry.Ready] where the SSH package is there; and where it is not,
 * [NotInstalled][SshEntry.NotInstalled] while the [catalogue] offers it or has not been read, [CantRun][SshEntry.CantRun]
 * where it has been and offers none that runs on the agent. [names] gives the saved head units' names by id; [switch]
 * is [SshAccess.state], taken only where it is of the head unit connected; [terminal] and [port] are taken where they
 * are of it.
 */
fun sshEntry(
    connection: ConnectionState,
    names: Map<String, String>,
    catalogue: CatalogueState,
    switchedOn: Set<String>,
    switch: SshSwitchState,
    terminal: TerminalState,
    port: SshPortState,
): SshEntry {
    val connected = connection as? ConnectionState.Connected ?: return SshEntry.NotConnected
    val id = connected.headUnitId
    val name = names[id] ?: UNNAMED
    if (!connected.channels) return SshEntry.CantRun(id, name)
    if (connected.installed.none { it.id == SshTunnel.PACKAGE }) {
        val known = catalogue.index != null
        return if (!known || sshOffered(catalogue, connected.installed, connected.info.agentVersion, switchedOn)) {
            SshEntry.NotInstalled(id, name)
        } else {
            SshEntry.CantRun(id, name)
        }
    }
    return SshEntry.Ready(
        id,
        name,
        on = switch.on.takeIf { switch.headUnitId == id },
        terminalOpen = terminal.active && terminal.headUnitId == id,
        port = port.takeIf { it.headUnitId == id } ?: SshPortState.Off(),
    )
}

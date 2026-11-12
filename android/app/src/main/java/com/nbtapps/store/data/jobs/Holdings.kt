// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.RestartKind

/**
 * The head unit the app is working with. There is no saved choice: it is the one the app's link is to, is being
 * opened to, or last failed to; with no such link, the one whose agent the app spoke to most recently; and
 * before any has been spoken to, the first that was saved. Null when none is saved.
 */
fun selectedHeadUnit(units: List<HeadUnit>, connection: ConnectionState): HeadUnit? =
    units.find { it.id == connection.headUnitId }
        ?: units.filter { it.profile != null }.maxByOrNull { it.profile!!.seenAt }
        ?: units.firstOrNull()

/** The head unit [this] state is about, in this app's ids. Null when there is no link and none was tried. */
val ConnectionState.headUnitId: String?
    get() = when (this) {
        ConnectionState.Disconnected -> null
        is ConnectionState.Connecting -> headUnitId
        is ConnectionState.Connected -> headUnitId
        is ConnectionState.NeedsPairing -> headUnitId
        is ConnectionState.Failed -> headUnitId
    }

/**
 * What the head unit the app is working with ([selectedHeadUnit]) holds, for the catalogue to be shown against:
 * as its agent says over the link that is open, or with no link, as it said the last time there was one.
 */
data class Holdings(
    /** This app's id of the head unit, as in [HeadUnit.id]. */
    val headUnitId: String,
    val name: String,
    /** Read over the link that is open now. Otherwise this is the saved profile, as old as [seenAt] says. */
    val live: Boolean,
    val agentVersion: String,
    val installed: List<InstalledPackage>,
    /**
     * What the head unit still has to restart for, as in [RestartKind]. Only a live one says: the agent keeps
     * the mark where a power down clears it, so a saved one is no longer true once the car has been off.
     */
    val restartNeeded: String,
    /** When this was read, in milliseconds since the epoch. */
    val seenAt: Long,
)

/**
 * The [Holdings] of the head unit the app is working with. Null when no head unit is saved, and when the one
 * the app is working with has never been spoken to: nothing is known of it then.
 */
fun holdings(units: List<HeadUnit>, connection: ConnectionState): Holdings? {
    val unit = selectedHeadUnit(units, connection) ?: return null
    val connected = (connection as? ConnectionState.Connected)?.takeIf { it.headUnitId == unit.id }
    val profile = unit.profile
    return when {
        connected != null -> Holdings(
            headUnitId = unit.id,
            name = unit.name,
            live = true,
            agentVersion = connected.info.agentVersion,
            installed = connected.installed,
            restartNeeded = connected.info.restartNeeded,
            // The profile is written with the same reading; it is absent only for the moment before that.
            seenAt = profile?.seenAt ?: 0,
        )
        profile != null -> Holdings(
            headUnitId = unit.id,
            name = unit.name,
            live = false,
            agentVersion = profile.agentVersion,
            installed = profile.installed,
            restartNeeded = RestartKind.NONE,
            seenAt = profile.seenAt,
        )
        else -> null
    }
}

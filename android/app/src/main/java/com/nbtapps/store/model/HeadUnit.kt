// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * A BMW NBT head unit the user has added. It is one car however it is reached, so it holds every way to
 * reach it, and the id its agent gives for itself once the two have spoken.
 */
@Serializable
data class HeadUnit(
    /** This app's own id for the entry. */
    val id: String,
    val name: String,
    val endpoints: List<Endpoint>,
    /**
     * The id the agent sent in `hello`: 32 hex characters, made by the agent on its first run. Null until
     * the first connection. A later connection that reports another id has reached a different head unit.
     */
    val headUnitId: String? = null,
    /** What the agent last reported, so the catalogue can be checked against this head unit at home. */
    val profile: HeadUnitProfile? = null,
    /**
     * What the agent's compatibility checks found, the one time they were run by themselves, and since then by
     * hand. Null until they have been answered: before the first link to an agent that has them.
     */
    val checks: HeadUnitChecks? = null,
    /**
     * The agent listed the capability `serial-dial` in its last `hello`: it has the car open a Bluetooth serial link
     * to the phone by itself, so a connect is given longer. False until a `hello` says so, and for a head unit saved
     * before there was this.
     */
    val serialDial: Boolean = false,
    /**
     * The agent listed the capability `serial-knock` in its last `hello`: it has the car dial the phone only when the
     * app knocks, so every connect to it knocks ([com.nbtapps.store.data.link.LinkConnector.knock]) and [phoneOpenedLeft]
     * no longer decides anything. Kept beside [serialDial], from the same `hello`. False until a `hello` says so, and for
     * a head unit saved before there was this. While it is false, a connect dials the agent as before, but tries its first
     * link by a probing knock before that dial (`JobRepository.greet`), which brings the car's link where the agent knows
     * knocks, and with its `hello` sets this.
     */
    val serialKnock: Boolean = false,
    /**
     * The agent said in its last `info.get` (`phoneOpenedLeft` in its `serial` object) that a Bluetooth serial link
     * this phone opens itself is left to another program on the car: a connect then only waits for the car's link
     * ([com.nbtapps.store.data.link.BluetoothSerialLinkConnector]). False until an `info.get` says so, where one leaves
     * it out, as an agent from before there was this does, and for a head unit saved before there was this.
     */
    val phoneOpenedLeft: Boolean = false,
)

/** What the agent's checks found on a head unit: see [HeadUnit.checks]. */
@Serializable
data class HeadUnitChecks(
    /** When they were answered, in milliseconds since the epoch. */
    val checkedAt: Long,
    /** The checks that found a problem, in the agent's order. Empty where all is well. */
    val problems: List<CheckProblem> = emptyList(),
) {
    /** The problems the user has not put aside. */
    val open: List<CheckProblem> get() = problems.filter { !it.dismissed }

    fun problem(id: String): CheckProblem? = problems.firstOrNull { it.id == id }
}

/**
 * A problem one check found, as it is kept with the head unit: the agent's own texts, and what it listed then. The
 * fix's token is not kept: it is good only for the list of the moment, so the check is run again before a fix. One
 * kept by an earlier version has no moves, and the packages it stood in the way of are no longer read.
 */
@Serializable
data class CheckProblem(
    /** The check's id, such as `aa-fix`. */
    val id: String,
    val title: String,
    val detail: String,
    /** The paths its fix removes, every one, as the agent showed them. Empty for a check that lists nothing. */
    val paths: List<String> = emptyList(),
    /** The folders its fix moves aside whole and leaves there. */
    val moves: List<CheckMove> = emptyList(),
    /** Its fix, if it has one for what it found. */
    val fix: CheckProblemFix? = null,
    /** The user put it aside. Running the checks again by hand brings it back. */
    val dismissed: Boolean = false,
) {
    /** Its fix changes files: it removes paths or moves folders aside, which are listed, and confirmed. */
    val changesFiles: Boolean get() = fix != null && (paths.isNotEmpty() || moves.isNotEmpty())

    companion object {
        /** [check], as it is kept; null for one that found no problem. */
        fun of(check: Check, dismissed: Boolean = false): CheckProblem? = if (!check.problem) {
            null
        } else {
            CheckProblem(
                id = check.id,
                title = check.title,
                detail = check.detail,
                paths = check.found,
                moves = check.moves,
                fix = check.fix?.let { CheckProblemFix(it.label, it.detail, it.restart) },
                dismissed = dismissed,
            )
        }
    }
}

/** A check's fix as it is kept: what its button says and what it does. */
@Serializable
data class CheckProblemFix(
    val label: String,
    val detail: String = "",
    /** One of [RestartKind]'s. */
    val restart: String = RestartKind.NONE,
)

/** One way to reach a head unit's agent. */
@Serializable
sealed interface Endpoint {
    /** A short description for a list row, such as `192.168.0.1:7144`. */
    val label: String
}

/**
 * The agent listens on a TCP port, and the phone dials it by the phone's own routing. Only the debug build has a
 * link for it, to the test bench; the agent has no TCP link on a car yet. Entries saved by earlier versions also
 * hold the network the socket was bound to (`transport`), which is no longer read.
 */
@Serializable
@SerialName("tcp")
data class TcpEndpoint(
    val host: String,
    val port: Int,
) : Endpoint {
    override val label: String get() = "$host:$port"
}

/** Bluetooth serial (RFCOMM) to the car, which must be paired with this phone. */
@Serializable
@SerialName("serial")
data class SerialEndpoint(
    /** The car's Bluetooth address. */
    val address: String,
    val deviceName: String,
) : Endpoint {
    override val label: String get() = "Bluetooth serial to $deviceName"
}

/**
 * These endpoints in the order to try them in: the TCP ones first, then Bluetooth serial, each kind in the order
 * given. A TCP connect that fails does so within seconds; a serial one is tried again for twenty, because another
 * app may hold the car's one serial link for a while, and it is the slower link when both would work.
 */
fun List<Endpoint>.inConnectOrder(): List<Endpoint> = sortedBy { it is SerialEndpoint }

/** What a head unit's agent reported the last time the app was connected to it. */
@Serializable
data class HeadUnitProfile(
    val agentVersion: String,
    /** The highest `manifestVersion` the agent installs. */
    val manifestVersion: Int,
    val free: FreeSpace,
    /** `none`, `hmi-restart` or `required`: whether an installed package still waits for a restart. */
    val restartNeeded: String,
    val installed: List<InstalledPackage>,
    /** When this was read, in milliseconds since the epoch. */
    val seenAt: Long,
)

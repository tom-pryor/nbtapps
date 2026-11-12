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

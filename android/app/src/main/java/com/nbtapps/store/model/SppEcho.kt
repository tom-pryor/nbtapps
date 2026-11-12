// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

/** How much data one echo test sends. */
enum class EchoSize(val label: String, val bytes: Int) {
    SMALL("16 KB", 16 * 1024),
    MEDIUM("256 KB", 256 * 1024),
    LARGE("1 MB", 1024 * 1024),
}

/** The outcome of an echo test that ran to the end. */
data class EchoReport(
    val bytesSent: Int,
    val bytesReceived: Int,
    /** Offset of the first byte that came back different from what was sent, or null if all matched. */
    val firstMismatchAt: Int?,
    /** Time for the first short message to come back. */
    val roundTripMillis: Long,
    /** Time from the first to the last byte of the whole exchange. */
    val totalMillis: Long,
) {
    val intact: Boolean
        get() = firstMismatchAt == null && bytesReceived == bytesSent

    /** Throughput of the echoed data. Each byte crosses the link twice, so one direction alone can be faster. */
    val kiloBytesPerSecond: Double
        get() = if (totalMillis > 0) bytesReceived / 1024.0 / (totalMillis / 1000.0) else 0.0
}

object SppEcho {
    /**
     * The service the head unit's echo test tool registers by default. A UUID of this project's own, so it
     * can't collide with the car's standard Bluetooth services; it must match the tool's.
     */
    const val DEFAULT_SERVICE_UUID = "AF1BC715-5EAA-46B0-88A7-76A34AFE7A88"

    private val UUID_PATTERN = Regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")

    /** Strict 8-4-4-4-12 check; java.util.UUID.fromString accepts shorter groups. */
    fun isValidUuid(text: String): Boolean = UUID_PATTERN.matches(text.trim())
}

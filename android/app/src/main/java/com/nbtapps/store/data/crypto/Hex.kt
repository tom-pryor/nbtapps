// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.crypto

import java.security.MessageDigest

/** Hex in the one form the protocol and the package format use: lowercase, two characters a byte. */
object Hex {
    private const val DIGITS = "0123456789abcdef"

    fun encode(bytes: ByteArray): String {
        val out = StringBuilder(bytes.size * 2)
        for (byte in bytes) {
            val value = byte.toInt() and 0xFF
            out.append(DIGITS[value shr 4]).append(DIGITS[value and 0xF])
        }
        return out.toString()
    }

    /** The bytes [text] spells, or null unless it is lowercase hex of exactly [byteCount] bytes. */
    fun decodeOrNull(text: String, byteCount: Int): ByteArray? {
        if (text.length != byteCount * 2) return null
        val out = ByteArray(byteCount)
        for (i in 0 until byteCount) {
            val high = DIGITS.indexOf(text[i * 2])
            val low = DIGITS.indexOf(text[i * 2 + 1])
            if (high < 0 || low < 0) return null
            out[i] = ((high shl 4) or low).toByte()
        }
        return out
    }

    /** Whether [text] is a SHA-256 hash as the manifest and the messages write one: 64 lowercase hex characters. */
    fun isSha256(text: String): Boolean = text.length == 64 && text.all { it in DIGITS }
}

fun newSha256(): MessageDigest = MessageDigest.getInstance("SHA-256")

fun sha256Hex(bytes: ByteArray): String = Hex.encode(newSha256().digest(bytes))

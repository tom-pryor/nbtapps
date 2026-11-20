// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.crypto.newSha256
import java.util.Base64
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * The public key lines the head unit's SSH relay takes, and the keys message they go in: the relay's rule
 * (`headunit/ssh/relay/key_lines.hpp`, and `headunit/ssh/README`, "The relay"), which this refuses exactly what the
 * relay refuses by. A line that passes here passes there, and the other way round.
 *
 * A line is `TYPE SP BASE64`, or `TYPE SP BASE64 SP COMMENT`:
 *  - TYPE is `ssh-ed25519` or `ecdsa-sha2-nistp256`, and comes first, so that no line can put `authorized_keys`
 *    options (`command="..."`, `no-pty`, `from=...`) in front of its key;
 *  - BASE64 is strict base64 (the standard alphabet, padded, no bits left over) of the key's SSH blob, whose first
 *    string is the same TYPE and whose rest is that type's key and nothing else: for `ssh-ed25519` a string of 32
 *    bytes, for `ecdsa-sha2-nistp256` the string `nistp256` and an uncompressed point of 65 bytes;
 *  - COMMENT, when there is one, is 1 to [MAX_COMMENT] characters;
 *  - every character of the line is printable ASCII (space to tilde): no control character, tab or line end, and
 *    nothing outside ASCII; so a line is at most [MAX_LINE] characters.
 *
 * A keys message holds 1 to [MAX_KEYS] lines.
 */
object SshKeyLines {
    const val ED25519 = "ssh-ed25519"
    const val ECDSA_P256 = "ecdsa-sha2-nistp256"

    /** The most lines in one keys message: the app's own key and those of other clients together. */
    const val MAX_KEYS = 8
    const val MAX_COMMENT = 128

    /** The longest line the rule lets through: the longer type, its blob of 104 bytes as 140 characters of base64, and the longest comment. */
    const val MAX_LINE = 19 + 1 + 140 + 1 + MAX_COMMENT

    /** Why [line] does not pass the rule, in words for the user; null when it passes. */
    fun problem(line: String): String? {
        if (line.isEmpty()) return "The line is empty"
        if (line.length > MAX_LINE) return "The line is too long"
        if (line.any { it < ' ' || it > '~' }) return "The line holds a character that is not printable ASCII"
        val typeEnd = line.indexOf(' ')
        val type = if (typeEnd < 0) line else line.substring(0, typeEnd)
        if (type != ED25519 && type != ECDSA_P256) return "The line does not start with $ED25519 or $ECDSA_P256"
        if (typeEnd < 0) return "The line has no key"
        val keyEnd = line.indexOf(' ', typeEnd + 1)
        val encoded = if (keyEnd < 0) line.substring(typeEnd + 1) else line.substring(typeEnd + 1, keyEnd)
        if (encoded.isEmpty()) return "The line has no key"
        val blob = decodeBase64(encoded) ?: return "The key is not base64"
        blobProblem(type, blob)?.let { return it }
        if (keyEnd >= 0) {
            val comment = line.length - keyEnd - 1
            if (comment == 0) return "The line ends in a space"
            if (comment > MAX_COMMENT) return "The comment is too long"
        }
        return null
    }

    fun passes(line: String): Boolean = problem(line) == null

    /**
     * A line as a user pastes it, made into one the relay takes where that can be done without changing the key:
     * spaces and line ends around it are dropped, and a comment the rule refuses (one with a character outside
     * ASCII, say, or one that is too long) is left off, since the key is all that logs in. Whatever else is wrong is
     * left for [problem] to say.
     */
    fun tidy(pasted: String): String {
        val line = pasted.trim()
        val typeEnd = line.indexOf(' ')
        if (typeEnd < 0) return line
        val keyEnd = line.indexOf(' ', typeEnd + 1)
        if (keyEnd < 0) return line
        val bare = line.substring(0, keyEnd)
        val comment = line.substring(keyEnd + 1).trim()
        if (problem(bare) != null) return line
        if (comment.isEmpty()) return bare
        val whole = "$bare $comment"
        return if (problem(whole) == null) whole else bare
    }

    /** The SSH blob of a line that passes the rule; null for one that does not. */
    fun blob(line: String): ByteArray? {
        if (!passes(line)) return null
        val typeEnd = line.indexOf(' ')
        val keyEnd = line.indexOf(' ', typeEnd + 1)
        return decodeBase64(if (keyEnd < 0) line.substring(typeEnd + 1) else line.substring(typeEnd + 1, keyEnd))
    }

    /** The key's type, as the line names it. */
    fun type(line: String): String = line.substringBefore(' ')

    /** A line's comment; null when it has none. */
    fun comment(line: String): String? {
        val typeEnd = line.indexOf(' ')
        val keyEnd = if (typeEnd < 0) -1 else line.indexOf(' ', typeEnd + 1)
        return if (keyEnd < 0) null else line.substring(keyEnd + 1)
    }

    /** The fingerprint OpenSSH shows for a key: `SHA256:` and the unpadded base64 of the SHA-256 of its blob. */
    fun fingerprint(blob: ByteArray): String =
        "SHA256:" + Base64.getEncoder().withoutPadding().encodeToString(newSha256().digest(blob))

    /**
     * The keys message, the first message on the relay's channel: `{"v":1,"keys":[...]}`, as UTF-8.
     *
     * @throws IllegalArgumentException with no line, more than [MAX_KEYS], or one that does not pass the rule.
     */
    fun keysMessage(lines: List<String>): ByteArray {
        require(lines.size in 1..MAX_KEYS) { "a keys message holds 1 to $MAX_KEYS lines, not ${lines.size}" }
        lines.forEachIndexed { i, line -> problem(line)?.let { throw IllegalArgumentException("line ${i + 1}: $it") } }
        return buildJsonObject {
            put("v", 1)
            put("keys", JsonArray(lines.map(::JsonPrimitive)))
        }.toString().encodeToByteArray()
    }

    /** SSH's own encoding of [parts]: each as a string, four bytes of its length, high byte first, then its bytes. */
    fun sshStrings(vararg parts: ByteArray): ByteArray {
        val out = ByteArray(parts.sumOf { 4 + it.size })
        var at = 0
        for (part in parts) {
            out[at] = (part.size ushr 24).toByte()
            out[at + 1] = (part.size ushr 16).toByte()
            out[at + 2] = (part.size ushr 8).toByte()
            out[at + 3] = part.size.toByte()
            part.copyInto(out, at + 4)
            at += 4 + part.size
        }
        return out
    }

    /** What is wrong with [blob] as a key of [type], or null when it is one, and nothing else. */
    private fun blobProblem(type: String, blob: ByteArray): String? {
        val reader = Strings(blob)
        val name = reader.next() ?: return "The key is cut short"
        if (!name.contentEquals(type.encodeToByteArray())) return "The key inside is not of the type the line names"
        if (type == ED25519) {
            val key = reader.next() ?: return "The key is cut short"
            if (key.size != 32) return "An $ED25519 key is not 32 bytes"
        } else {
            val curve = reader.next() ?: return "The key is cut short"
            if (!curve.contentEquals("nistp256".encodeToByteArray())) return "The key's curve is not nistp256"
            val point = reader.next() ?: return "The key is cut short"
            if (point.size != 65 || point[0] != 4.toByte()) return "The key's point is not 65 bytes, uncompressed"
        }
        if (!reader.atEnd) return "The key has bytes after its end"
        return null
    }

    /** Reads SSH strings off a blob, one after another. */
    private class Strings(private val blob: ByteArray) {
        private var at = 0
        val atEnd: Boolean get() = at == blob.size

        fun next(): ByteArray? {
            if (blob.size - at < 4) return null
            val length = ((blob[at].toLong() and 0xFF) shl 24) or ((blob[at + 1].toLong() and 0xFF) shl 16) or
                ((blob[at + 2].toLong() and 0xFF) shl 8) or (blob[at + 3].toLong() and 0xFF)
            at += 4
            if (length > blob.size - at) return null
            return blob.copyOfRange(at, at + length.toInt()).also { at += length.toInt() }
        }
    }

    private const val ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

    /** Strict base64: null for any text that is not the one encoding of some bytes. */
    internal fun decodeBase64(text: String): ByteArray? {
        if (text.isEmpty() || text.length % 4 != 0) return null
        val padding = when {
            text[text.length - 1] != '=' -> 0
            text[text.length - 2] == '=' -> 2
            else -> 1
        }
        val out = ByteArray(text.length / 4 * 3 - padding)
        var o = 0
        for (i in text.indices step 4) {
            val last = i + 4 == text.length
            var group = 0
            for (k in 0 until 4) {
                val c = text[i + k]
                val value = if (last && k >= 4 - padding) {
                    if (c != '=') return null
                    0
                } else {
                    ALPHABET.indexOf(c).takeIf { it >= 0 } ?: return null
                }
                group = (group shl 6) or value
            }
            out[o++] = (group shr 16).toByte()
            if (!last || padding < 2) out[o++] = (group shr 8).toByte()
            if (!last || padding < 1) out[o++] = group.toByte()
            // The bits the padding leaves over are 0 in the one encoding of these bytes.
            if (last && padding == 2 && (group and 0xFFFF) != 0) return null
            if (last && padding == 1 && (group and 0xFF) != 0) return null
        }
        return out
    }
}

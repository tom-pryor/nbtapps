// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.json.StrictJson
import java.util.Base64
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * The relay's key-line rule as the app holds it ([SshKeyLines]). The cases are those of the relay's own test,
 * `headunit/tests/ssh/test_key_lines.cpp`, so the two refuse the same lines. The keys are invented: their bytes count
 * up from 0 or 1, so only their layout is that of a real key; the base64 below was written by another encoder.
 */
class SshKeyLinesTest {

    private val ed = "AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g"
    private val ec =
        "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8="
    private val rsa =
        "AAAAB3NzaC1yc2EAAAADAQABAAAAgQABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9" +
            "Pj9AQUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVpbXF1eX2BhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gA=="

    private fun encode(bytes: ByteArray) = Base64.getEncoder().encodeToString(bytes)

    private fun string(bytes: ByteArray) = SshKeyLines.sshStrings(bytes)

    private fun string(text: String) = string(text.encodeToByteArray())

    private fun accepted(line: String) = assertNull("refused: $line", SshKeyLines.problem(line))

    private fun refused(line: String, what: String) {
        val why = SshKeyLines.problem(line)
        assertNotNull("not refused, $what: $line", why)
        assertTrue("a reason for $what", why!!.isNotEmpty())
    }

    @Test
    fun `lines of the two types pass, with a comment or without`() {
        accepted("ssh-ed25519 $ed")
        accepted("ssh-ed25519 $ed laptop")
        accepted("ssh-ed25519 $ed with spaces and (punctuation) ~!@#$%^&*")
        accepted("ecdsa-sha2-nistp256 $ec")
        accepted("ecdsa-sha2-nistp256 $ec phone@example")
        accepted("ssh-ed25519 $ed " + "c".repeat(SshKeyLines.MAX_COMMENT))
    }

    @Test
    fun `nothing may come before the type, and the type is the blob's`() {
        refused("command=\"/bin/true\" ssh-ed25519 $ed", "an option before the type")
        refused("no-pty ssh-ed25519 $ed", "no-pty before the type")
        refused("from=\"10.0.0.1\",no-pty ssh-ed25519 $ed x", "options before the type")
        refused(" ssh-ed25519 $ed", "a space before the type")
        refused("ecdsa-sha2-nistp256 $ed", "an ed25519 blob named as ecdsa")
        refused("ssh-ed25519 $ec", "an ecdsa blob named as ed25519")
        refused("ssh-rsa $rsa", "an RSA key")
        refused("ssh-ed25519 $rsa", "an RSA blob named as ed25519")
        refused("ssh-dss $ed", "an unknown type")
        refused("SSH-ED25519 $ed", "the type in capitals")
    }

    @Test
    fun `the key is strict base64 of exactly a key of its type`() {
        refused("ssh-ed25519 " + ed.dropLast(1), "base64 cut by one character")
        refused("ssh-ed25519 $ed=", "base64 with a padding character too many")
        refused("ssh-ed25519 " + ed.substring(0, 10) + "*" + ed.substring(11), "a character outside base64")
        // ec ends in "8=": the 2 bits of '8' that the padding leaves over are 0, and of '9' they are not.
        refused("ecdsa-sha2-nistp256 " + ec.dropLast(2) + "9=", "padded base64 with bits left over")
        refused("ssh-ed25519 =" + ed.substring(1), "padding at the start")

        val key = string("ssh-ed25519") + string(ByteArray(32) { 1 })
        accepted("ssh-ed25519 " + encode(key))
        refused("ssh-ed25519 " + encode(key.copyOf(key.size - 1)), "a blob one byte short")
        refused("ssh-ed25519 " + encode(key.copyOf(15)), "a blob of the type's name alone")
        refused("ssh-ed25519 " + encode(key + 'x'.code.toByte()), "a blob with a byte after the key")
        refused("ssh-ed25519 " + encode(string("ssh-ed25519") + string(ByteArray(31) { 'k'.code.toByte() })), "an ed25519 key of 31 bytes")
        refused("ssh-ed25519 " + encode(string("ssh-ed25519") + string(ByteArray(33) { 'k'.code.toByte() })), "an ed25519 key of 33 bytes")
        val point = byteArrayOf(4) + ByteArray(64) { 'p'.code.toByte() }
        accepted("ecdsa-sha2-nistp256 " + encode(string("ecdsa-sha2-nistp256") + string("nistp256") + string(point)))
        refused("ecdsa-sha2-nistp256 " + encode(string("ecdsa-sha2-nistp256") + string("nistp384") + string(point)), "an ecdsa key on another curve")
        refused(
            "ecdsa-sha2-nistp256 " + encode(string("ecdsa-sha2-nistp256") + string("nistp256") + string(byteArrayOf(2) + ByteArray(32) { 'p'.code.toByte() })),
            "a compressed point",
        )
        refused("ssh-ed25519 " + encode(byteArrayOf(-1, -1, -1, -1) + ByteArray(40) { 'x'.code.toByte() }), "a blob with a huge length")
    }

    @Test
    fun `the line is printable ASCII, one space between its parts, and not too long`() {
        refused("", "an empty line")
        refused("ssh-ed25519", "a type alone")
        refused("ssh-ed25519 ", "a type and a space")
        refused("ssh-ed25519  $ed", "two spaces after the type")
        refused("ssh-ed25519 $ed ", "a space at the end")
        refused("ssh-ed25519\t$ed", "a tab after the type")
        refused("ssh-ed25519 $ed\tlaptop", "a tab before the comment")
        refused("ssh-ed25519 $ed lap\ttop", "a tab in the comment")
        refused("ssh-ed25519 $ed laptop\n", "a line end")
        refused("ssh-ed25519 $ed\nssh-ed25519 $ed", "two lines in one")
        refused("ssh-ed25519 $ed lap\rtop", "a carriage return")
        refused("ssh-ed25519 $ed lap\u001btop", "an escape")
        refused("ssh-ed25519 $ed lap\u007ftop", "a delete")
        refused("ssh-ed25519 $ed café", "a character outside ASCII")
        refused("ssh-ed25519 $ed\u0000x", "a nul character")
        refused("ssh-ed25519 $ed " + "c".repeat(SshKeyLines.MAX_COMMENT + 1), "a comment one too long")
        refused("ssh-ed25519 $ed " + "c".repeat(10_000), "a line far too long")
        refused("A".repeat(SshKeyLines.MAX_LINE + 1), "a line of base64 characters only")
        assertEquals(289, SshKeyLines.MAX_LINE)
    }

    @Test
    fun `strict base64 is the one encoding of its bytes`() {
        assertEquals("Man", SshKeyLines.decodeBase64("TWFu")?.decodeToString())
        assertEquals("Ma", SshKeyLines.decodeBase64("TWE=")?.decodeToString())
        assertEquals("M", SshKeyLines.decodeBase64("TQ==")?.decodeToString())
        for (bad in listOf("TR==", "TWF", "", "====", "T===", "TQ==TWFu", "TWE")) assertNull(bad, SshKeyLines.decodeBase64(bad))
        val all = ByteArray(256) { it.toByte() }
        for (n in 1..all.size step 37) {
            val part = all.copyOf(n)
            assertArrayEquals(part, SshKeyLines.decodeBase64(encode(part)))
        }
    }

    @Test
    fun `the keys message is one JSON object of 1 to 8 lines that pass`() {
        val one = "ssh-ed25519 $ed one"
        val two = "ecdsa-sha2-nistp256 $ec"
        assertEquals("""{"v":1,"keys":["$one","$two"]}""", SshKeyLines.keysMessage(listOf(one, two)).decodeToString())

        // A comment's quotes and backslashes go escaped, and read back as they were.
        val quoted = """ssh-ed25519 $ed say "hi" \ there"""
        val read = StrictJson.parseObject(SshKeyLines.keysMessage(listOf(quoted)))["keys"] as JsonArray
        assertEquals(quoted, read.single().jsonPrimitive.content)

        // Eight of the longest lines, every character of their comments escaped, fit the largest message.
        val longest = "ecdsa-sha2-nistp256 $ec " + "\"".repeat(SshKeyLines.MAX_COMMENT)
        assertEquals(SshKeyLines.MAX_LINE, longest.length)
        assertTrue(SshKeyLines.keysMessage(List(8) { longest }).size <= 4096)

        for (bad in listOf(emptyList(), List(9) { one }, listOf(one, "no-pty $one"))) {
            try {
                SshKeyLines.keysMessage(bad)
                fail("taken: $bad")
            } catch (e: IllegalArgumentException) {
                // As the relay would refuse it.
            }
        }
    }

    @Test
    fun `a pasted line is tidied, the spaces around it go and a comment the relay would refuse is left off`() {
        assertEquals("ssh-ed25519 $ed laptop", SshKeyLines.tidy("  ssh-ed25519 $ed laptop\r\n"))
        assertEquals("ssh-ed25519 $ed", SshKeyLines.tidy("ssh-ed25519 $ed café"))
        assertEquals("ssh-ed25519 $ed", SshKeyLines.tidy("ssh-ed25519 $ed " + "c".repeat(200)))
        assertEquals("ssh-ed25519 $ed", SshKeyLines.tidy("ssh-ed25519 $ed   "))
        assertEquals("ssh-ed25519 $ed two  words", SshKeyLines.tidy("ssh-ed25519 $ed two  words"))
        // What is wrong with the type or the key is left for the rule to say.
        assertEquals("no-pty ssh-ed25519 $ed", SshKeyLines.tidy("no-pty ssh-ed25519 $ed"))
        assertEquals("ssh-rsa $rsa me", SshKeyLines.tidy("ssh-rsa $rsa me"))
    }

    @Test
    fun `a line gives its blob, type, comment and the fingerprint OpenSSH shows`() {
        val line = "ssh-ed25519 $ed test"
        assertArrayEquals(Base64.getDecoder().decode(ed), SshKeyLines.blob(line))
        assertNull(SshKeyLines.blob("ssh-rsa $rsa"))
        assertEquals("ssh-ed25519", SshKeyLines.type(line))
        assertEquals("test", SshKeyLines.comment(line))
        assertNull(SshKeyLines.comment("ssh-ed25519 $ed"))
        // As `ssh-keygen -lf` gives it.
        assertEquals("SHA256:mKqU+0K8OhKmA8bBQi9Rz0Q5l7/g160hIP+rJYSTNj4", SshKeyLines.fingerprint(SshKeyLines.blob(line)!!))
    }
}

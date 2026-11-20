// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The other SSH clients' keys, kept on the phone ([DataStoreAuthorizedClients]) and in memory. */
class AuthorizedClientsTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val file by lazy { File(folder.root, "ssh-clients.json") }

    /** Runs [block] on a store over the file, then closes it, as [com.nbtapps.store.data.PrereleaseStoreTest] does. */
    private suspend fun <T> opened(block: suspend (AuthorizedClients) -> T): T {
        val job = Job()
        try {
            return block(DataStoreAuthorizedClients(authorizedClientsStore(file, CoroutineScope(Dispatchers.IO + job))))
        } finally {
            job.cancelAndJoin()
        }
    }

    /** An invented ed25519 key whose 32 bytes are all [n]. */
    private fun key(n: Int, comment: String? = null): String {
        val blob = SshKeyLines.sshStrings("ssh-ed25519".encodeToByteArray(), ByteArray(32) { n.toByte() })
        return "ssh-ed25519 " + java.util.Base64.getEncoder().encodeToString(blob) + (comment?.let { " $it" } ?: "")
    }

    @Test
    fun `starts with none, keeps what is added for a store that opens later, and removes by fingerprint`() = runTest {
        opened { clients ->
            assertTrue(clients.clients.first().isEmpty())
            assertEquals(AddClientResult.Added(ClientKey("Laptop", key(1, "me@laptop"))), clients.add(" Laptop ", key(1, "me@laptop") + "\n"))
            assertEquals(AddClientResult.Added(ClientKey("Phone app", key(2))), clients.add("Phone app", key(2)))
        }
        assertEquals("""[{"name":"Laptop","line":"${key(1, "me@laptop")}"},{"name":"Phone app","line":"${key(2)}"}]""", file.readText())

        opened { clients ->
            assertEquals(listOf("Laptop", "Phone app"), clients.clients.first().map { it.name })
            clients.remove(ClientKey("x", key(1)).fingerprint)
            // Removing one that is not there changes nothing.
            clients.remove("SHA256:nothing")
        }
        assertEquals(listOf(ClientKey("Phone app", key(2))), opened { it.clients.first() })
    }

    @Test
    fun `a line the relay would refuse, a bad name, a key kept already, and one too many are refused`() = runTest {
        opened { clients ->
            assertTrue(clients.add("Old", "ssh-rsa AAAAB3NzaC1yc2E= old") is AddClientResult.BadLine)
            assertTrue(clients.add("Options", "no-pty " + key(1)) is AddClientResult.BadLine)
            assertEquals(AddClientResult.BadName, clients.add("  ", key(1)))
            assertEquals(AddClientResult.BadName, clients.add("n".repeat(AuthorizedClients.MAX_NAME + 1), key(1)))
            assertTrue(clients.add("n".repeat(AuthorizedClients.MAX_NAME), key(1)) is AddClientResult.Added)
            // The same key under another comment is the same key.
            assertEquals(AddClientResult.Already("n".repeat(AuthorizedClients.MAX_NAME)), clients.add("Again", key(1, "other")))
            for (n in 2..AuthorizedClients.MAX_CLIENTS) assertTrue(clients.add("Key $n", key(n)) is AddClientResult.Added)
            assertEquals(AddClientResult.Full, clients.add("One too many", key(99)))
            assertEquals(7, clients.clients.first().size)
        }
    }

    @Test
    fun `a pasted line is tidied, a comment the relay would refuse left off`() = runTest {
        opened { clients ->
            assertEquals(AddClientResult.Added(ClientKey("Café", key(3))), clients.add("Café", "  " + key(3, "café") + "\r\n"))
        }
        // The name is the user's: it is not sent to the head unit.
        assertEquals(listOf(key(3)), opened { it.clients.first() }.map { it.line })
    }

    @Test
    fun `a file this version cannot read is no keys, and entries the rule refuses are left out`() = runTest {
        file.writeText("""{"keys": true}""")
        opened { clients ->
            assertTrue(clients.clients.first().isEmpty())
            clients.add("Laptop", key(1))
        }
        assertEquals(listOf("Laptop"), opened { it.clients.first() }.map { it.name })

        file.writeText("""[{"name":"Old","line":"ssh-rsa AAAA old"},{"name":"Laptop","line":"${key(1)}"},{"name":"Twice","line":"${key(1)}"},{"name":"","line":"${key(2)}"}]""")
        assertEquals(listOf(ClientKey("Laptop", key(1))), opened { it.clients.first() })
    }

    @Test
    fun `the in-memory store behaves the same, and the keys message has the app's key first`() = runTest {
        val clients = InMemoryAuthorizedClients()
        assertTrue(clients.add("Laptop", key(1)) is AddClientResult.Added)
        assertTrue(clients.add("Tablet", key(2)) is AddClientResult.Added)
        assertEquals(AddClientResult.Already("Laptop"), clients.add("Laptop", key(1)))
        clients.remove(ClientKey("Tablet", key(2)).fingerprint)
        assertEquals(listOf("Laptop"), clients.clients.first().map { it.name })

        val app = SshKey.generate()
        assertEquals(listOf(app.publicKeyLine(), key(1)), clients.keysWith(MemorySshIdentity(app)))
    }
}

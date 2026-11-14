// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

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

/** The packages switched on for pre-release versions, kept on the phone ([DataStorePrereleaseRepository]). */
class PrereleaseStoreTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val file by lazy { File(folder.root, "prerelease-packages.json") }

    /** Runs [block] on a repository over the file, then closes its store, as [HeadUnitStoreTest] does. */
    private suspend fun <T> opened(block: suspend (PrereleaseRepository) -> T): T {
        val job = Job()
        try {
            return block(DataStorePrereleaseRepository(prereleaseStore(file, CoroutineScope(Dispatchers.IO + job))))
        } finally {
            job.cancelAndJoin()
        }
    }

    @Test
    fun `starts with every switch off, and keeps one turned on for a repository that starts later`() = runTest {
        opened { first ->
            assertTrue(first.switchedOn.first().isEmpty())
            first.set("org.example.radio", true)
            first.set("org.nbtapp.agent", true)
            // Turning one on twice is the same as once.
            first.set("org.example.radio", true)
        }

        assertEquals(setOf("org.example.radio", "org.nbtapp.agent"), opened { it.switchedOn.first() })
        assertEquals("""["org.example.radio","org.nbtapp.agent"]""", file.readText())
    }

    @Test
    fun `a switch stays on until it is turned off`() = runTest {
        opened { it.set("org.example.radio", true) }
        assertEquals(setOf("org.example.radio"), opened { it.switchedOn.first() })

        opened { repository ->
            repository.set("org.example.radio", false)
            // Turning off one that is not on changes nothing.
            repository.set("org.example.other", false)
        }
        assertTrue(opened { it.switchedOn.first() }.isEmpty())
    }

    @Test
    fun `a file this version cannot read leaves every switch off`() = runTest {
        file.writeText("""{"radio": true}""")

        opened { repository ->
            assertTrue(repository.switchedOn.first().isEmpty())
            repository.set("org.example.radio", true)
        }

        assertEquals(setOf("org.example.radio"), opened { it.switchedOn.first() })
    }

    @Test
    fun `the in-memory repository behaves the same`() = runTest {
        val memory = InMemoryPrereleaseRepository()
        memory.set("a", true)
        memory.set("b", true)
        memory.set("a", false)

        assertEquals(setOf("b"), memory.switchedOn.first())
    }
}

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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Whether the user has skipped the set-up, kept on the phone ([DataStoreSetupRepository]). */
class SetupStoreTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val file by lazy { File(folder.root, "setup.json") }

    /** Runs [block] on a repository over the file, then closes its store, as [HeadUnitStoreTest] does. */
    private suspend fun <T> opened(block: suspend (SetupRepository) -> T): T {
        val job = Job()
        try {
            return block(DataStoreSetupRepository(setupStore(file, CoroutineScope(Dispatchers.IO + job))))
        } finally {
            job.cancelAndJoin()
        }
    }

    @Test
    fun `starts not skipped, and keeps a skip for a repository that starts later`() = runTest {
        opened { first ->
            assertFalse(first.skipped.first())
            first.setSkipped(true)
        }

        assertTrue(opened { it.skipped.first() })
        assertEquals("""{"skipped":true}""", file.readText())
    }

    @Test
    fun `a skip stays until it is changed back`() = runTest {
        opened { it.setSkipped(true) }
        opened { repository ->
            assertTrue(repository.skipped.first())
            repository.setSkipped(false)
        }

        assertFalse(opened { it.skipped.first() })
    }

    @Test
    fun `a file this version cannot read is taken as not skipped, and one with more in it is still read`() = runTest {
        file.writeText("""["skipped"]""")
        opened { repository ->
            assertFalse(repository.skipped.first())
            repository.setSkipped(true)
        }
        assertTrue(opened { it.skipped.first() })

        // As a later version might write it.
        file.writeText("""{"skipped":true,"step":"pair"}""")
        assertTrue(opened { it.skipped.first() })
    }

    @Test
    fun `the in-memory repository behaves the same`() = runTest {
        val memory = InMemorySetupRepository()
        assertFalse(memory.skipped.first())
        memory.setSkipped(true)
        assertTrue(memory.skipped.first())
        memory.setSkipped(false)
        assertFalse(memory.skipped.first())
    }
}

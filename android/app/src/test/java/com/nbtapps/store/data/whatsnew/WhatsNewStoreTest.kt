// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.whatsnew

import androidx.datastore.core.DataStore
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** What has been seen of what is new, and the kept notes, on the phone ([whatsNewStore]). */
class WhatsNewStoreTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val file by lazy { File(folder.root, "whats-new.json") }

    private val notes = VersionNotes(900, "0.9.0", "2026-11-23", listOf("A terminal on the head unit", "SSH access for other apps"))
    private val next = VersionNotes(1000, "1.0.0", null, listOf("The next one"))

    /** Runs [block] on a store over the file, then closes it, as the other stores' tests do. */
    private suspend fun <T> opened(block: suspend (DataStore<WhatsNewSettings>) -> T): T {
        val job = Job()
        try {
            return block(whatsNewStore(file, CoroutineScope(Dispatchers.IO + job)))
        } finally {
            job.cancelAndJoin()
        }
    }

    @Test
    fun `starts empty, and keeps what is seen and the notes for a store that opens later`() = runTest {
        opened { store ->
            assertEquals(WhatsNewSettings(), store.data.first())
            store.updateData { it.copy(seen = 802, notes = listOf(notes, next)) }
        }

        assertEquals(WhatsNewSettings(seen = 802, notes = listOf(notes, next)), opened { it.data.first() })
        assertEquals(
            """{"format":1,"seen":802,"notes":[{"versionCode":900,"versionName":"0.9.0","released":"2026-11-23",""" +
                """"lines":["A terminal on the head unit","SSH access for other apps"]},""" +
                """{"versionCode":1000,"versionName":"1.0.0","released":null,"lines":["The next one"]}]}""",
            file.readText(),
        )
    }

    @Test
    fun `a file this version cannot read is taken as empty, and replaced at the next write`() = runTest {
        val unreadables = listOf(
            """["seen"]""", "{", "", """{"seen":"yesterday"}""", """{"notes":[{"versionName":"0.9.0"}]}""",
            // Notes as one object, as the file was laid out before it held more than one.
            """{"format":1,"seen":802,"notes":{"versionCode":900,"versionName":"0.9.0","lines":["A terminal"]}}""",
            """{"format":1,"seen":802,"notes":null}""",
        )
        for (unreadable in unreadables) {
            file.writeText(unreadable)
            assertEquals(unreadable, WhatsNewSettings(), opened { it.data.first() })
        }
        file.writeBytes(byteArrayOf(0xff.toByte(), 0xfe.toByte(), 0x00, 0x7b))
        opened { store ->
            assertEquals(WhatsNewSettings(), store.data.first())
            store.updateData { it.copy(seen = 900) }
        }
        assertEquals(WhatsNewSettings(seen = 900), opened { it.data.first() })
    }

    @Test
    fun `a file of a later format is taken as empty, and one with more in it is still read`() = runTest {
        file.writeText("""{"format":2,"seen":1200}""")
        assertEquals(WhatsNewSettings(), opened { it.data.first() })

        // As a later version of the same format might write it.
        file.writeText("""{"format":1,"seen":900,"shownAt":1763900000000}""")
        assertEquals(WhatsNewSettings(seen = 900), opened { it.data.first() })
    }
}

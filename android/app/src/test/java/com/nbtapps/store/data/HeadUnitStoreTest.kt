// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import com.nbtapps.store.model.CheckMove
import com.nbtapps.store.model.CheckProblem
import com.nbtapps.store.model.CheckProblemFix
import com.nbtapps.store.model.FreeSpace
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitChecks
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class HeadUnitStoreTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val file by lazy { File(folder.root, "head-units.json") }

    private val car = HeadUnit(
        id = "one",
        name = "Car",
        endpoints = listOf(TcpEndpoint("192.168.0.1", 7144), SerialEndpoint("00:11:22:33:44:55", "BMW 12345")),
    )

    /**
     * Runs [block] on a repository over the file, then closes its store: only one may be open on a file, and
     * a store opened afterwards stands for the app starting again.
     */
    private suspend fun <T> opened(block: suspend (HeadUnitRepository) -> T): T {
        val job = Job()
        try {
            return block(DataStoreHeadUnitRepository(headUnitStore(file, CoroutineScope(Dispatchers.IO + job))))
        } finally {
            job.cancelAndJoin()
        }
    }

    @Test
    fun `starts empty, and keeps what is added for a repository that starts later`() = runTest {
        opened { first ->
            assertTrue(first.headUnits.first().isEmpty())
            first.add(car)
            first.add(car.copy(id = "two", name = "Other car", endpoints = emptyList()))
        }

        opened { second ->
            assertEquals(listOf("Car", "Other car"), second.headUnits.first().map { it.name })
            assertEquals(car, second.headUnit("one").first())
            assertNull(second.headUnit("three").first())
        }
    }

    @Test
    fun `the agent's id and its last profile are kept with the head unit`() = runTest {
        val profile = HeadUnitProfile(
            agentVersion = "1.0.0",
            manifestVersion = 1,
            free = FreeSpace(app = 10, data = 20, plugins = 30, staging = 40),
            restartNeeded = "hmi-restart",
            installed = listOf(InstalledPackage("org.nbtapp.runtime", "NBT Runtime", "2.1.0", "ab".repeat(32), listOf("runs-scripts"))),
            seenAt = 1_794_000_000_000,
        )
        opened { first ->
            first.add(car)
            first.update("one") { it.copy(headUnitId = "000102030405060708090a0b0c0d0e0f", profile = profile) }
            first.update("nobody") { it.copy(name = "never") }
        }

        val saved = opened { it.headUnit("one").first() }
        assertEquals("000102030405060708090a0b0c0d0e0f", saved?.headUnitId)
        assertEquals(profile, saved?.profile)
        assertEquals(car.endpoints, saved?.endpoints)
    }

    @Test
    fun `what the checks found is kept with the head unit, and one saved before there were checks loads without`() = runTest {
        val checks = HeadUnitChecks(
            checkedAt = 1_794_000_000_000,
            problems = listOf(
                CheckProblem(
                    id = "aa-fix",
                    title = "An app that was installed by hand",
                    detail = "It stands in the way.",
                    paths = listOf("/opt/demo/bin/demod"),
                    moves = listOf(CheckMove("/plugins/Demo", "/opt/demo/Demo.old")),
                    fix = CheckProblemFix("Remove it", "Removes what was listed.", "hmi-restart"),
                    dismissed = true,
                ),
            ),
        )
        opened { first ->
            first.add(car)
            first.update("one") { it.copy(checks = checks) }
        }
        assertEquals(checks, opened { it.headUnit("one").first() }?.checks)

        file.writeText("""[{"id":"one","name":"Car","endpoints":[]}]""")
        assertNull(opened { it.headUnit("one").first() }?.checks)

        // As the version before moves wrote it: with the packages a problem stood in the way of, and no moves.
        file.writeText(
            """[{"id":"one","name":"Car","endpoints":[],"checks":{"checkedAt":1,"problems":[{"id":"aa-fix","title":"T",""" +
                """"detail":"D","paths":["/opt/demo/bin/demod"],"blocks":["org.nbtapp.plugin"],"fix":{"label":"L"}}]}}]""",
        )
        val older = requireNotNull(opened { it.headUnit("one").first() }?.checks?.problem("aa-fix"))
        assertEquals(emptyList<CheckMove>(), older.moves)
        assertEquals(listOf("/opt/demo/bin/demod"), older.paths)
    }

    @Test
    fun `removing a head unit is kept too`() = runTest {
        opened { first ->
            first.add(car)
            first.remove("one")
        }
        assertTrue(opened { it.headUnits.first() }.isEmpty())
    }

    /** Picks [device] from [repository] [times] times at once, as a double tap would, and returns the ids told. */
    private suspend fun pickedAtOnce(repository: HeadUnitRepository, device: PairedDevice, times: Int): List<String> = coroutineScope {
        (1..times).map { async(Dispatchers.Default) { repository.saveCar(device) } }.awaitAll()
    }

    @Test
    fun `a car is saved once however often it is picked, and one saved before is found by its address`() = runTest {
        val device = PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF", car = true)
        opened { repository ->
            val ids = pickedAtOnce(repository, device, 2)
            val unit = repository.headUnits.first().single()
            assertEquals(listOf(unit.id, unit.id), ids)
            assertEquals(HeadUnit(unit.id, "BMW 12345", listOf(SerialEndpoint("AA:BB:CC:DD:EE:FF", "BMW 12345"))), unit)
        }

        opened { repository ->
            repository.add(car)
            // The same car under another name and with its address in small letters, as it might be saved by hand.
            assertEquals("one", repository.saveCar(PairedDevice("Other name", "00:11:22:33:44:55")))
            assertEquals(2, repository.headUnits.first().size)
            assertEquals(car, repository.headUnit("one").first())
        }
    }

    @Test
    fun `the in-memory repository saves a car once too`() = runTest {
        val memory = InMemoryHeadUnitRepository()
        val device = PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF")
        repeat(20) { pickedAtOnce(memory, device, 4) }

        val unit = memory.headUnits.first().single()
        assertEquals(unit.id, memory.saveCar(device.copy(address = "aa:bb:cc:dd:ee:ff")))
        // A head unit with no serial endpoint is no car to look for.
        val bench = HeadUnit("bench", "Bench", listOf(TcpEndpoint("192.168.0.1", 7144)))
        assertEquals("bench", memory.addUnlessSaved(bench))
        assertEquals("bench2", memory.addUnlessSaved(bench.copy(id = "bench2")))
        assertEquals(3, memory.headUnits.first().size)
    }

    @Test
    fun `a head unit saved with the network its address was bound to still loads`() = runTest {
        // As versions before the transport choice was taken out wrote it.
        file.writeText(
            """[{"id":"one","name":"Car","endpoints":[""" +
                """{"type":"tcp","host":"192.168.0.1","port":7144,"transport":"BLUETOOTH_PAN"},""" +
                """{"type":"serial","address":"00:11:22:33:44:55","deviceName":"BMW 12345"}]}]""",
        )

        assertEquals(listOf(car), opened { it.headUnits.first() })
    }

    @Test
    fun `a file this version cannot read is set aside, not lost`() = runTest {
        file.writeText("""[{"id":"one","name":"Car","host":"192.168.0.1","port":22,"transport":"WIFI"}]""")

        opened { repository ->
            assertTrue(repository.headUnits.first().isEmpty())
            repository.add(car)
        }

        assertTrue(File(file.path + ".unreadable").readText().contains("\"host\""))
        assertEquals(listOf(car), opened { it.headUnits.first() })
    }

    @Test
    fun `the phone's id is made once and then kept`() = runTest {
        val idFile = File(folder.root, "phone-id")
        val first = FilePhoneIdentity(idFile, "Pixel 9")
        val id = first.id()

        assertTrue(Regex("[0-9a-f]{32}").matches(id))
        assertEquals(id, first.id())
        assertEquals(id, FilePhoneIdentity(idFile, "Pixel 9").id())

        // A file that holds no id is replaced by a new one.
        idFile.writeText("not an id")
        val replaced = FilePhoneIdentity(idFile, "Pixel 9").id()
        assertTrue(Regex("[0-9a-f]{32}").matches(replaced))
        assertNotEquals("not an id", replaced)
    }
}

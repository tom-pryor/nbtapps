// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemoryPrereleaseRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.FakeCheck
import com.nbtapps.store.data.jobs.FakeChecks
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.holdings
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.CheckMove
import com.nbtapps.store.model.CheckState
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.ui.headunits.CHECK_FILES_CHANGED
import com.nbtapps.store.ui.headunits.CheckFixStep
import com.nbtapps.store.ui.headunits.checkFixStopped
import com.nbtapps.store.ui.headunits.checkFixSummary
import com.nbtapps.store.ui.headunits.movesHeading
import com.nbtapps.store.ui.headunits.removesHeading
import com.nbtapps.store.ui.headunits.checksLine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The head unit's page with what the agent's checks found: the card, the confirmation of a fix, and Check again. */
class HeadUnitChecksTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy { JobRepository(headUnits, connector, identity, MemoryPairingKeys(), store, {}, scope, now = { 1234L }) }
    private val setup by lazy {
        val source = FileCatalogueSource(null, AgentVectors.file("packages/good"))
        AutostartSetup(jobs, DefaultCatalogueRepository(source), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
    }

    private val car = HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144)))
    private val demoFiles = listOf("/opt/demo/bin/demod", "/opt/demo/etc/demo.conf", "/var/opt/demo/state")
    private val demoMove = "/net/plugins/Demo" to "/opt/demo/Demo.old"

    /** What the stand-in car's checks find, on every link to it. */
    private val checks = FakeChecks().also { shared ->
        connector.onConnect = { agent ->
            agent.capabilities = """["checks"]"""
            agent.checks = shared
        }
    }

    @After
    fun tearDown() = scope.cancel()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(car)
            block()
        }
    }

    private fun viewModel() = HeadUnitDetailViewModel("car", headUnits, jobs, connector, FakePhoneBluetooth(), setup, scope)

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun connected(viewModel: HeadUnitDetailViewModel): HeadUnitDetailUiState {
        assertTrue(jobs.connect("car").isSuccess)
        return await(viewModel.uiState, "the checks on the page") { it.canCheck && it.checks != null }
    }

    @Test
    fun `what the checks found is on the page, and a fix is confirmed against the list as it is now`() = test {
        checks.add(FakeCheck("aa-fix", demoFiles, moves = listOf(demoMove)))
        val viewModel = viewModel()
        val page = connected(viewModel)
        assertTrue(showsChecks(page, connected = true))
        assertEquals(demoFiles, page.checks?.open?.single()?.paths)
        assertEquals(listOf(CheckMove("/net/plugins/Demo", "/opt/demo/Demo.old")), page.checks?.open?.single()?.moves)

        // A file came since the checks ran: the confirmation shows it, since the check is read again first.
        val now = demoFiles + "/opt/demo/lib/libdemo.so"
        checks.problems.getValue("aa-fix").found = now
        viewModel.openFix("aa-fix")
        val confirm = await(viewModel.uiState, "the confirmation") { it.fix is CheckFixStep.Confirm }.fix as CheckFixStep.Confirm
        assertEquals(now.sorted(), confirm.check.found)
        assertEquals(1, confirm.check.moves.size)
        assertFalse(confirm.changed)
        assertTrue(checks.fixes.isEmpty())

        viewModel.confirmFix()
        val fixed = await(viewModel.uiState, "the outcome") { it.fix is CheckFixStep.Fixed }.fix as CheckFixStep.Fixed
        assertEquals(CheckState.OK, fixed.result.state)
        assertEquals("1 folder was moved aside. 4 paths were removed.", checkFixSummary(fixed.check, fixed.result))
        assertEquals("Stopped first: demod.", checkFixStopped(fixed.result))
        assertEquals(listOf("aa-fix" to checks.token("aa-fix", now, listOf(demoMove))), checks.fixes)

        // The problem is gone from the page, and the agent says iDrive has to restart.
        val after = await(viewModel.uiState, "the page after the fix") { it.checks?.open?.isEmpty() == true && it.restartNeeded }
        assertEquals(RestartKind.HMI, after.profile?.restartNeeded)
        viewModel.closeFix()
        assertNull(await(viewModel.uiState, "the dialog closed") { it.fix == null }.fix)
    }

    @Test
    fun `a list that changed after it was shown is refused, read again and shown with a line that says so`() = test {
        val problem = FakeCheck("aa-fix", demoFiles)
        checks.add(problem)
        val viewModel = viewModel()
        connected(viewModel)
        viewModel.openFix("aa-fix")
        await(viewModel.uiState, "the confirmation") { it.fix is CheckFixStep.Confirm }

        problem.found = demoFiles + "/opt/demo/new"
        viewModel.confirmFix()

        val again = await(viewModel.uiState, "the new list") { (it.fix as? CheckFixStep.Confirm)?.changed == true }.fix as CheckFixStep.Confirm
        assertEquals((demoFiles + "/opt/demo/new").sorted(), again.check.found)
        assertTrue(CHECK_FILES_CHANGED.isNotEmpty())
        assertEquals(listOf("/opt/demo/new"), problem.found - demoFiles.toSet())
        viewModel.confirmFix()
        await(viewModel.uiState, "the outcome") { it.fix is CheckFixStep.Fixed }
        assertEquals(2, checks.fixes.size)
    }

    @Test
    fun `a fix that fails says why and can be tried again`() = test {
        checks.add(FakeCheck("aa-fix", demoFiles))
        checks.refuseFix["aa-fix"] = "io-error"
        val viewModel = viewModel()
        connected(viewModel)
        viewModel.openFix("aa-fix")
        await(viewModel.uiState, "the confirmation") { it.fix is CheckFixStep.Confirm }

        viewModel.confirmFix()
        val failed = await(viewModel.uiState, "the failure") { it.fix is CheckFixStep.Failed }.fix as CheckFixStep.Failed
        assertTrue(failed.reason, failed.reason.contains("couldn't remove"))

        checks.refuseFix.clear()
        viewModel.retryFix()
        await(viewModel.uiState, "the confirmation again") { it.fix is CheckFixStep.Confirm }
    }

    @Test
    fun `dismiss puts a problem aside, and check again brings it back`() = test {
        checks.add(FakeCheck("aa-fix", demoFiles))
        val viewModel = viewModel()
        connected(viewModel)

        viewModel.dismissCheck("aa-fix")
        val aside = await(viewModel.uiState, "put aside") { it.checks?.open?.isEmpty() == true }
        // The card stays for Check again while there is a link.
        assertTrue(showsChecks(aside, connected = true))
        assertFalse(showsChecks(aside.copy(canCheck = false), connected = false))

        viewModel.recheck()
        val back = await(viewModel.uiState, "back") { it.checks?.open?.size == 1 && !it.checking }
        assertNull(back.checkError)
        assertEquals(2, checks.runs.get())
    }

    @Test
    fun `with no link the problems are still shown, and the catalogue's banner counts them`() = test {
        checks.add(FakeCheck("aa-fix", demoFiles))
        checks.add(FakeCheck("system-writable", emptyList(), fixLabel = "Make it read-only again", restart = "none"))
        val viewModel = viewModel()
        connected(viewModel)
        jobs.disconnect()

        val offline = await(viewModel.uiState, "the page with no link") { !it.canCheck }
        assertEquals(listOf("system-writable", "aa-fix"), offline.checks?.open?.map { it.id })
        assertTrue(showsChecks(offline, connected = false))
        // A fix that changes no files has no list to show.
        assertFalse(offline.checks!!.problem("system-writable")!!.changesFiles)

        val held = requireNotNull(holdings(headUnits.headUnits.first(), jobs.connection.value))
        assertEquals(2, held.openChecks)
        assertEquals("2 things on Test car need a look.", checksLine(held.openChecks, held.name))
        assertEquals("1 thing on Test car needs a look.", checksLine(1, held.name))
        assertEquals("Show the 3 paths to remove", listLabel(0, 3, shown = false))
        assertEquals("Show the 1 folder to move aside and 1 path to remove", listLabel(1, 1, shown = false))
        assertEquals("Show the 2 folders to move aside", listLabel(2, 0, shown = false))
        assertEquals("Hide the list", listLabel(1, 3, shown = true))
        assertEquals("This folder is moved aside, not removed, and kept where it goes:", movesHeading(1))
        assertEquals("These 3 are removed from the head unit:", removesHeading(3))
    }

    @Test
    fun `an agent without the checks shows no card`() = test {
        connector.onConnect = { it.capabilities = """["autostart"]""" }
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        val page = await(viewModel.uiState, "connected") { it.link is LinkState.Connected }
        assertFalse(page.canCheck)
        assertFalse(showsChecks(page, connected = true))
    }
}

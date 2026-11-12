// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.jobs.selectedHeadUnit
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.FreeSpace
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.JobSnapshot
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
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

class HeadUnitIndicatorTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val tcp = TcpEndpoint("192.168.0.1", 7144)
    private val car = HeadUnit("car", "Car", listOf(tcp))
    private val van = HeadUnit("van", "Van", listOf(TcpEndpoint("192.168.0.2", 7144)))

    /** A head unit this build has no way to: the stand-in connector reaches TCP only. */
    private val bike = HeadUnit("bike", "Bike", listOf(SerialEndpoint("00:11:22:33:44:55", "Bike")))

    private val tcpOnly = { endpoint: Endpoint -> endpoint is TcpEndpoint }

    private fun seen(unit: HeadUnit, at: Long) =
        unit.copy(profile = HeadUnitProfile("1.0.0", 1, FreeSpace(1, 1, 1, 1), "none", emptyList(), seenAt = at))

    private fun connected(id: String) =
        ConnectionState.Connected(id, tcp, AgentInfo("1.0.0", "000102030405060708090a0b0c0d0e0f", 1), emptyList())

    private fun state(units: List<HeadUnit>, connection: ConnectionState, job: JobView? = null) =
        indicatorState(units, connection, job, tcpOnly)

    @Test
    fun `with no head unit saved there is none to show`() {
        val none = state(emptyList(), ConnectionState.Disconnected)
        assertTrue(none.loaded)
        assertNull(none.headUnit)
        // A state left over from a head unit that was removed since changes nothing.
        assertNull(state(emptyList(), ConnectionState.Failed("car", "No route")).headUnit)
    }

    @Test
    fun `with no link, the head unit is the one spoken to last, and before any was, the first saved`() {
        assertEquals("car", selectedHeadUnit(listOf(car, van), ConnectionState.Disconnected)?.id)
        assertEquals("van", selectedHeadUnit(listOf(car, seen(van, 5)), ConnectionState.Disconnected)?.id)
        assertEquals("car", selectedHeadUnit(listOf(seen(car, 9), seen(van, 5)), ConnectionState.Disconnected)?.id)
    }

    @Test
    fun `the head unit the link is to, is opened to or failed to comes before the one spoken to last`() {
        val units = listOf(seen(car, 9), van)
        assertEquals("van", selectedHeadUnit(units, ConnectionState.Connecting("van"))?.id)
        assertEquals("van", selectedHeadUnit(units, connected("van"))?.id)
        assertEquals("van", selectedHeadUnit(units, ConnectionState.Failed("van", "No route"))?.id)
        // Not one that is no longer saved.
        assertEquals("car", selectedHeadUnit(units, ConnectionState.Failed("gone", "No route"))?.id)
    }

    @Test
    fun `each state of the link has its dot and its line`() {
        val units = listOf(car)

        val idle = state(units, ConnectionState.Disconnected)
        assertEquals(IndicatorHeadUnit("car", "Car"), idle.headUnit)
        assertEquals(IndicatorStatus.IDLE, idle.status)
        assertEquals("Not connected", idle.detail)
        assertTrue(idle.canConnect)

        val connecting = state(units, ConnectionState.Connecting("car"))
        assertEquals(IndicatorStatus.CONNECTING, connecting.status)

        val up = state(units, connected("car"))
        assertEquals(IndicatorStatus.CONNECTED, up.status)
        assertEquals("Connected: 192.168.0.1:7144", up.detail)

        val failed = state(units, ConnectionState.Failed("car", "No route to the car"))
        assertEquals(IndicatorStatus.FAILED, failed.status)
        assertEquals("No route to the car", failed.detail)
        assertTrue(failed.canConnect)

        // A link that waits for the phone to be paired is the head unit's as a connected one is.
        val unpaired = state(listOf(van, car), ConnectionState.NeedsPairing("car", tcp, stale = false))
        assertEquals("car", unpaired.headUnit?.id)
        assertEquals(IndicatorStatus.NEEDS_PAIRING, unpaired.status)
        assertTrue(unpaired.detail, unpaired.detail.contains("isn't paired"))
        assertFalse(unpaired.busy)
        val forgotten = state(units, ConnectionState.NeedsPairing("car", tcp, stale = true))
        assertEquals(IndicatorStatus.NEEDS_PAIRING, forgotten.status)
        assertTrue(forgotten.detail, forgotten.detail.contains("no longer knows"))
    }

    @Test
    fun `a state that names a head unit no longer saved is not shown on another`() {
        val other = state(listOf(car), ConnectionState.Failed("gone", "No route"))
        assertEquals("car", other.headUnit?.id)
        assertEquals(IndicatorStatus.IDLE, other.status)
        assertEquals("Not connected", other.detail)
    }

    @Test
    fun `a head unit this build cannot reach is shown, with no connect, and is not offered among the others`() {
        val only = state(listOf(bike), ConnectionState.Disconnected)
        assertEquals("bike", only.headUnit?.id)
        assertFalse(only.canConnect)
        assertEquals("This version of the app can't open a link to this head unit", only.detail)

        val among = state(listOf(seen(car, 1), van, bike), ConnectionState.Disconnected)
        assertEquals(listOf(IndicatorHeadUnit("van", "Van")), among.others)
    }

    @Test
    fun `a job running over the link marks the head unit busy, and one that ended or lost its link does not`() {
        val units = listOf(car, van)
        fun job(state: String, on: String = "car", linkLost: Boolean = false) =
            JobView("1011121314151617", on, plan = null, job = JobSnapshot("1011121314151617", state), linkLost = linkLost)

        assertTrue(state(units, connected("car"), job(JobState.INSTALLING)).busy)
        assertFalse(state(units, connected("car"), job(JobState.SUCCEEDED)).busy)
        assertFalse(state(units, connected("car"), null).busy)
        // The last job was on the other head unit.
        assertFalse(state(units, connected("car"), job(JobState.INSTALLING, on = "van")).busy)
        // The link went, and the job waits on the head unit: connecting elsewhere takes nothing from it.
        assertFalse(state(units, ConnectionState.Failed("car", "The link was lost"), job(JobState.PAUSED, linkLost = true)).busy)
    }

    // The view model over the real job repository and a stand-in agent.

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val jobs by lazy { JobRepository(headUnits, connector, identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), {}, scope, now = { 1234L }) }
    private val viewModel by lazy { HeadUnitIndicatorViewModel(headUnits, jobs, connector, scope) }

    @After
    fun tearDown() = scope.cancel()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            val collecting = launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
            block()
            collecting.cancel()
        }
    }

    private suspend fun await(what: String, predicate: (HeadUnitIndicatorUiState) -> Boolean): HeadUnitIndicatorUiState =
        withTimeoutOrNull(10_000) { viewModel.uiState.first(predicate) }
            ?: throw AssertionError("never came: $what; last was ${viewModel.uiState.value}")

    @Test
    fun `connecting and disconnecting from the menu show on the indicator, and the head unit stays the one shown`() = test {
        headUnits.add(van)
        headUnits.add(car)
        assertEquals("van", await("the first saved") { it.headUnit != null }.headUnit?.id)

        viewModel.connect("car")
        val up = await("the link") { it.status == IndicatorStatus.CONNECTED }
        assertEquals("car", up.headUnit?.id)
        assertEquals(listOf(IndicatorHeadUnit("van", "Van")), up.others)

        viewModel.disconnect()
        val down = await("no link") { it.status == IndicatorStatus.IDLE }
        // It was spoken to last, so it is still the one, though the van was saved first.
        assertEquals("car", down.headUnit?.id)
        assertEquals("Not connected", down.detail)
    }

    @Test
    fun `a connect that fails shows why`() = test {
        headUnits.add(car)
        connector.unreachable = "No route to the car"

        viewModel.connect("car")

        val failed = await("the failure") { it.status == IndicatorStatus.FAILED }
        assertEquals("No route to the car", failed.detail)
        assertTrue(failed.canConnect)
    }
}

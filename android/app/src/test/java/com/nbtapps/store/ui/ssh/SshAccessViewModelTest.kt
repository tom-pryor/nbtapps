// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.ssh

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.ssh.AddClientResult
import com.nbtapps.store.data.ssh.AuthorizedClients
import com.nbtapps.store.data.ssh.ClientKey
import com.nbtapps.store.data.ssh.InMemoryAuthorizedClients
import com.nbtapps.store.data.ssh.PortAddress
import com.nbtapps.store.data.ssh.PortChannel
import com.nbtapps.store.data.ssh.PortLink
import com.nbtapps.store.data.ssh.PortOffReason
import com.nbtapps.store.data.ssh.PortTunnel
import com.nbtapps.store.data.ssh.SshKeyLines
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.ssh.SwitchReading
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.ChannelSwitch
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.withTestTimeout
import java.io.IOException
import java.util.Base64
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** "SSH from other apps": each state the screen shows, the commands and their labels, and adding and removing keys. */
class SshAccessViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))

    private fun key(n: Int, name: String = "Key $n") = ClientKey(
        name,
        "ssh-ed25519 " + Base64.getEncoder().encodeToString(
            SshKeyLines.sshStrings("ssh-ed25519".encodeToByteArray(), ByteArray(32) { (it + n).toByte() }),
        ),
    )

    private val laptop = key(1, "Laptop")
    private val phoneApp = key(2, "SSH app on this phone")

    private fun on(
        connections: Int = 0,
        otherDevices: Boolean = false,
        paused: Boolean = false,
        addresses: List<PortAddress> = listOf(PortAddress(PortAddress.Kind.THIS_PHONE, "127.0.0.1")),
    ) = SshPortState.On(CAR, 2222, otherDevices, connections, paused, addresses, hostKey = "SHA256:pQ7wZ3nR0xKd8LmYtV2sHcA9eGuJ4bFiO6yTqN1rWXE")

    private fun ui(
        port: SshPortState = SshPortState.Off(),
        keys: List<ClientKey> = listOf(laptop, phoneApp),
        reach: SshReach = SshReach.READY,
    ) = SshAccessUiState(name = "Test car", reach = reach, keys = keys, port = port, headUnitId = CAR)

    // ---- The states, as the frames draw them ----

    @Test
    fun `1, no key saved, the port cannot be switched on`() {
        val first = ui(keys = emptyList())
        assertEquals("Add a key first", first.switchLine)
        assertFalse(first.switchEnabled)
        assertFalse(first.switchOn)
        assertFalse(first.otherDevicesEnabled)
        assertTrue(first.commands.isEmpty())
        assertTrue(first.canAdd)
    }

    @Test
    fun `2 and 3, the add dialog, and each refusal of a key with the dialog kept open`() {
        val form = AddKeyForm("Laptop", "ssh-ed25519 AAAA laptop password")
        assertEquals(form.copy(keyError = BAD_LINE), addRefusal(AddClientResult.BadLine("The key is not base64"), form))
        assertEquals(
            "That isn't a public key line. Paste the whole line, starting with ssh-ed25519 or ecdsa-sha2-nistp256.",
            addRefusal(AddClientResult.BadLine("x"), form)?.keyError,
        )
        assertEquals(form.copy(nameError = BAD_NAME), addRefusal(AddClientResult.BadName, form))
        assertEquals("That key is saved already, as Laptop.", addRefusal(AddClientResult.Already("Laptop"), form)?.keyError)
        assertEquals(KEYS_FULL, addRefusal(AddClientResult.Full, form)?.keyError)
        assertNull(addRefusal(AddClientResult.Added(laptop), form))
    }

    @Test
    fun `4, keys saved and the port off`() {
        val off = ui()
        assertEquals("Off", off.switchLine)
        assertTrue(off.switchEnabled)
        assertFalse(off.otherDevicesEnabled)
        assertEquals("Lets a laptop on this phone's hotspot or Wi-Fi connect. Can be set while the port is on.", off.otherDevicesLine)
        assertNull(off.on)
        assertNull(off.offReason)
        assertNull(off.failure)
    }

    @Test
    fun `5, switching on, the switch on and waiting`() {
        val checking = ui(port = SshPortState.Checking(CAR))
        assertEquals("Checking the way to the head unit…", checking.switchLine)
        assertTrue(checking.switchOn)
        assertFalse(checking.switchEnabled)
        assertFalse(checking.otherDevicesEnabled)
        assertNull(checking.on)
    }

    @Test
    fun `6, on for this phone only, one command and the host key`() {
        val state = ui(port = on())
        assertEquals("On · port 2222 · no connections", state.switchLine)
        assertTrue(state.switchOn)
        assertTrue(state.switchEnabled)
        assertTrue(state.otherDevicesEnabled)
        assertEquals("Lets a laptop on this phone's hotspot or Wi-Fi connect", state.otherDevicesLine)
        assertEquals(listOf(PortCommand(null, "ssh -p 2222 root@127.0.0.1")), state.commands)
        assertEquals("On · port 2222 · 1 connection", ui(port = on(1)).switchLine)
        assertEquals("On · port 2222 · 2 connections", ui(port = on(2)).switchLine)
    }

    @Test
    fun `7, other devices allowed, a command for each address with its label`() {
        val wide = ui(
            port = on(
                connections = 2,
                otherDevices = true,
                addresses = listOf(
                    PortAddress(PortAddress.Kind.THIS_PHONE, "127.0.0.1"),
                    PortAddress(PortAddress.Kind.HOTSPOT, "192.0.2.1"),
                    PortAddress(PortAddress.Kind.WIFI, "198.51.100.23"),
                    PortAddress(PortAddress.Kind.NETWORK, "203.0.113.5"),
                ),
            ),
        )
        assertEquals(
            listOf(
                PortCommand("This phone", "ssh -p 2222 root@127.0.0.1", "Copy command for this phone"),
                PortCommand("Hotspot", "ssh -p 2222 root@192.0.2.1", "Copy command for the hotspot"),
                PortCommand("Wi-Fi", "ssh -p 2222 root@198.51.100.23", "Copy command for Wi-Fi"),
                PortCommand("Network", "ssh -p 2222 root@203.0.113.5", "Copy command for the network"),
            ),
            wide.commands,
        )
        assertEquals(true, wide.on?.otherDevices)
        assertEquals(
            "Anyone on this phone's hotspot or Wi-Fi can reach the port. Only the keys below let them in.",
            OTHER_DEVICES_WARNING,
        )
    }

    @Test
    fun `8, paused by a job`() {
        assertTrue(ui(port = on(1, paused = true)).paused)
        assertFalse(ui(port = on(1)).paused)
        assertFalse(ui().paused)
    }

    @Test
    fun `9, went off by itself, each reason in its place`() {
        fun reason(why: PortOffReason) = ui(port = SshPortState.Off(CAR, reason = why))
        assertEquals("Turned off after 30 minutes with no connection", reason(PortOffReason.IDLE).offReason)
        assertEquals("Turned off: the link to the head unit was lost", reason(PortOffReason.LINK_LOST).offReason)
        assertEquals("Turned off: SSH access was switched off", reason(PortOffReason.SWITCHED_OFF).offReason)
        assertEquals("Turned off: the SSH package changed", reason(PortOffReason.PACKAGE_CHANGED).offReason)
        assertEquals("Off", reason(PortOffReason.IDLE).switchLine)
        assertNull(reason(PortOffReason.IDLE).failure)
    }

    @Test
    fun `10, could not switch on, the reason in the error colour`() {
        val failed = ui(port = SshPortState.Off(CAR, failure = "The head unit is busy with a job. Try again once it has finished."))
        assertEquals("The head unit is busy with a job. Try again once it has finished.", failed.failure)
        assertNull(failed.offReason)
        assertFalse(failed.switchOn)
        assertTrue(failed.switchEnabled)
    }

    @Test
    fun `11, SSH access off or no link, the switch greyed and the keys still editable`() {
        val sshOff = ui(reach = SshReach.SSH_OFF)
        assertEquals("Switch on SSH access first", sshOff.switchLine)
        assertFalse(sshOff.switchEnabled)
        assertTrue(sshOff.canAdd)
        val apart = ui(reach = SshReach.NO_LINK)
        assertEquals("Connect to the head unit first", apart.switchLine)
        assertFalse(apart.switchEnabled)
        assertTrue(apart.canAdd)
    }

    @Test
    fun `12, the remove question names the key`() {
        assertEquals(laptop, ui().copy(removing = laptop).removing)
    }

    @Test
    fun `13, seven keys, and Add key greyed`() {
        val full = ui(keys = (1..AuthorizedClients.MAX_CLIENTS).map { key(it) })
        assertFalse(full.canAdd)
        assertEquals("7 keys at most. Remove one to add another.", KEYS_FULL)
        assertTrue(ui(keys = (1..6).map { key(it) }).canAdd)
    }

    @Test
    fun `the port on for another head unit shows here as off, its switch greyed`() {
        val elsewhere = ui(port = SshPortState.On("other", 2222, hostKey = "SHA256:x"))
        assertTrue(elsewhere.elsewhere)
        assertNull(elsewhere.on)
        assertFalse(elsewhere.switchOn)
        assertFalse(elsewhere.switchEnabled)
        assertEquals("On for another head unit", elsewhere.switchLine)
        assertTrue(elsewhere.commands.isEmpty())
    }

    @Test
    fun `a key's row shows its fingerprint shortened`() {
        assertEquals("SHA256:Hf3Kq9Zx…YbQ6", shortFingerprint("SHA256:Hf3Kq9Zxabcdefghijklmnopqrstuvwxyz0123YbQ6"))
        assertEquals("SHA256:short", shortFingerprint("SHA256:short"))
    }

    // ---- The view model ----

    private val connection = MutableStateFlow<ConnectionState>(
        ConnectionState.Connected(
            CAR,
            TcpEndpoint("192.0.2.1", 7144),
            AgentInfo("1.7.0", "000102030405060708090a0b0c0d0e0f", 1),
            listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0")),
            capabilities = listOf(ProtocolConstants.CAPABILITY_CHANNELS),
        ),
    )

    /** A link that never gets as far as a session: switching on stays at its check while [gate] is open. */
    private inner class StuckLink : PortLink {
        override val connection: StateFlow<ConnectionState> get() = this@SshAccessViewModelTest.connection
        override val jobOn = flowOf<String?>(null)
        val gate = CompletableDeferred<Unit>()
        val entered = CompletableDeferred<Unit>()

        override suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T> {
            entered.complete(Unit)
            gate.await()
            return Result.failure(IOException("Not connected to a head unit"))
        }
    }

    private object NoChannel : PortChannel {
        override suspend fun open(session: AgentSession, keys: List<String>): PortTunnel = throw IOException("no")
        override suspend fun switch(session: AgentSession) = SwitchReading.UNKNOWN
    }

    private val clients = InMemoryAuthorizedClients()
    private val link = StuckLink()
    private val port = SshPort(link, NoChannel, clients, {}, scope)
    private val reads = AtomicInteger()

    @Volatile
    private var sshOn = true

    /** A job on the head unit, during which the agent answers nothing of SSH access. */
    private val jobRuns = MutableStateFlow(false)

    /** Whether Android still wants the Nearby devices permission before other devices are let in. */
    @Volatile
    private var needed = false

    private fun viewModel() = mainDispatcherRule.track(
        SshAccessViewModel(
            CAR, port, clients, connection, flowOf("Test car"),
            readSwitch = {
                reads.incrementAndGet()
                if (jobRuns.value) Result.failure(IOException("busy")) else Result.success(ChannelSwitch(switched = true, on = sshOn, open = 0))
            },
            jobRuns = jobRuns,
            localNetworkNeeded = { needed },
        ),
    )

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        try {
            withTestTimeout(30_000, block)
        } finally {
            link.gate.complete(Unit)
        }
    }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    @Test
    fun `the head unit is ready with a link and SSH access on, and not with it off or no link`() = test {
        val viewModel = viewModel()
        assertEquals(SshReach.READY, await(viewModel.uiState, "ready") { it.reach == SshReach.READY }.reach)
        assertEquals("Test car", viewModel.uiState.value.name)

        connection.value = ConnectionState.Disconnected
        await(viewModel.uiState, "no link") { it.reach == SshReach.NO_LINK }

        sshOn = false
        connection.value = ConnectionState.Connected(
            CAR, TcpEndpoint("192.0.2.1", 7144), AgentInfo("1.7.0", "000102030405060708090a0b0c0d0e0f", 1),
            listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0")), capabilities = listOf(ProtocolConstants.CAPABILITY_CHANNELS),
        )
        await(viewModel.uiState, "SSH off") { it.reach == SshReach.SSH_OFF }
        assertTrue(reads.get() >= 2)
    }

    @Test
    fun `adding a key, each refusal keeping the dialog open with its line, and the question before removing one`() = test {
        val viewModel = viewModel()
        viewModel.openAdd()
        await(viewModel.uiState, "the dialog") { it.adding == AddKeyForm() }

        viewModel.editAdd(AddKeyForm("Laptop", "ssh-ed25519 AAAA laptop password"))
        viewModel.confirmAdd()
        assertEquals(BAD_LINE, await(viewModel.uiState, "a bad line") { it.adding?.keyError != null }.adding?.keyError)
        // Typing again puts the line away.
        viewModel.editAdd(AddKeyForm("", laptop.line))
        assertNull(await(viewModel.uiState, "edited") { it.adding?.line == laptop.line }.adding?.keyError)
        viewModel.confirmAdd()
        assertEquals(BAD_NAME, await(viewModel.uiState, "no name") { it.adding?.nameError != null }.adding?.nameError)

        viewModel.editAdd(AddKeyForm("Laptop", laptop.line))
        viewModel.confirmAdd()
        val added = await(viewModel.uiState, "added") { it.adding == null && it.keys.isNotEmpty() }
        assertEquals(listOf("Laptop"), added.keys.map { it.name })

        viewModel.openAdd()
        viewModel.editAdd(AddKeyForm("Again", laptop.line))
        viewModel.confirmAdd()
        assertEquals("That key is saved already, as Laptop.", await(viewModel.uiState, "already") { it.adding?.keyError != null }.adding?.keyError)
        viewModel.cancelAdd()
        await(viewModel.uiState, "closed") { it.adding == null }

        // Full at seven: Add key is greyed and opens nothing.
        (2..7).forEach { assertTrue(clients.add("Key $it", key(it).line) is AddClientResult.Added) }
        await(viewModel.uiState, "seven") { it.keys.size == 7 && !it.canAdd }
        viewModel.openAdd()
        assertNull(viewModel.uiState.value.adding)

        // Remove asks first; Cancel keeps it, Remove takes it away.
        val first = viewModel.uiState.value.keys.first()
        viewModel.askRemove(first)
        await(viewModel.uiState, "the question") { it.removing == first }
        viewModel.cancelRemove()
        await(viewModel.uiState, "kept") { it.removing == null && it.keys.size == 7 }
        viewModel.askRemove(first)
        viewModel.confirmRemove()
        val left = await(viewModel.uiState, "removed") { it.keys.size == 6 }
        assertFalse(first in left.keys)
        assertTrue(left.canAdd)
    }

    @Test
    fun `switching on goes to the port, and while it is on for another head unit it is refused here with a line`() = test {
        assertTrue(clients.add("Laptop", laptop.line) is AddClientResult.Added)
        val here = connection.value
        connection.value = (here as ConnectionState.Connected).copy(headUnitId = "other")
        assertTrue(port.switchOn("other"))
        // Past its look at the link, it waits at the check.
        withTimeoutOrNull(10_000) { link.entered.await() } ?: throw AssertionError("the check never began")
        assertEquals(SshPortState.Checking("other"), port.state.value)
        connection.value = here
        val viewModel = viewModel()
        val elsewhere = await(viewModel.uiState, "elsewhere") { it.elsewhere && it.reach == SshReach.READY }
        assertFalse(elsewhere.switchEnabled)
        viewModel.setPort(true)
        assertEquals(SshPortState.Checking("other"), port.state.value)

        // Once it is off, switching on here goes to the port.
        port.switchOff()
        link.gate.complete(Unit)
        await(viewModel.uiState, "off") { !it.elsewhere && it.switchEnabled }
        viewModel.setPort(true)
        val failed = await(viewModel.uiState, "the port's answer") { it.failure != null }
        assertEquals("Not connected to a head unit", failed.failure)
        assertEquals(CAR, port.state.value.headUnitId)
    }

    @Test
    fun `allowing other devices asks for the permission first where Android wants it, and never to stop`() {
        assertEquals(OtherDevicesStep.ASK, otherDevicesStep(allow = true, enabled = true, needed = true))
        assertEquals(OtherDevicesStep.SET, otherDevicesStep(allow = true, enabled = true, needed = false))
        assertEquals(OtherDevicesStep.SET, otherDevicesStep(allow = false, enabled = true, needed = true))
        assertEquals(OtherDevicesStep.NOTHING, otherDevicesStep(allow = true, enabled = false, needed = true))
    }

    @Test
    fun `a refusal of the permission says why and what to do, asked again or in the phone's settings`() {
        assertNull(localNetworkRefusal(granted = true, askAgain = false))
        assertEquals(LocalNetworkRefusal.ASK_AGAIN, localNetworkRefusal(granted = false, askAgain = true))
        assertEquals(LocalNetworkRefusal.FOR_GOOD, localNetworkRefusal(granted = false, askAgain = false))
        assertEquals(
            "Other devices need the Nearby devices permission, which lets this app take connections from the phone's hotspot or " +
                "Wi-Fi. Switch Allow other devices on again to be asked once more.",
            ui().copy(localNetworkRefusal = LocalNetworkRefusal.ASK_AGAIN).localNetworkLine,
        )
        assertEquals(
            "Other devices need the Nearby devices permission, and Android won't ask for it again. Allow Nearby devices in this " +
                "app's permissions in the phone's settings, then switch Allow other devices on.",
            ui().copy(localNetworkRefusal = LocalNetworkRefusal.FOR_GOOD).localNetworkLine,
        )
        assertNull(ui().localNetworkLine)
    }

    @Test
    fun `Android's answer is kept as the screen shows it, and a grant puts any refusal away`() = test {
        val viewModel = viewModel()
        viewModel.localNetworkAnswered(granted = false, askAgain = true)
        await(viewModel.uiState, "asked again") { it.localNetworkRefusal == LocalNetworkRefusal.ASK_AGAIN }
        viewModel.localNetworkAnswered(granted = false, askAgain = false)
        await(viewModel.uiState, "for good") { it.localNetworkRefusal == LocalNetworkRefusal.FOR_GOOD }
        viewModel.localNetworkAnswered(granted = true, askAgain = false)
        val granted = await(viewModel.uiState, "granted") { it.localNetworkRefusal == null }
        assertFalse(granted.askLocalNetwork)
        // With the port off nothing is asked, whatever Android wants.
        needed = true
        viewModel.setOtherDevices(true)
        assertFalse(viewModel.uiState.value.askLocalNetwork)
    }

    @Test
    fun `during a job the screen says so, and reads SSH access again once it has ended`() = test {
        assertTrue(clients.add("Laptop", laptop.line) is AddClientResult.Added)
        jobRuns.value = true
        val viewModel = viewModel()
        val during = await(viewModel.uiState, "the job") { it.reach == SshReach.JOB && it.keys.isNotEmpty() }
        assertEquals("A job runs on the head unit. The port can be switched on once it has finished.", during.switchLine)
        assertFalse(during.switchEnabled)

        jobRuns.value = false
        assertEquals(SshReach.READY, await(viewModel.uiState, "ready") { it.reach == SshReach.READY }.reach)

        // Known to be on before a job: it stays ready.
        jobRuns.value = true
        delay(200)
        assertEquals(SshReach.READY, viewModel.uiState.value.reach)
    }

    @Test
    fun `other devices are set only while the port is on`() = test {
        val viewModel = viewModel()
        viewModel.setOtherDevices(true)
        assertTrue(port.state.value is SshPortState.Off)
        assertFalse(viewModel.uiState.value.otherDevicesEnabled)
    }

    private companion object {
        const val CAR = "car"
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.crypto.sha256Hex
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.LinkConnectors
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.AutostartReason
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.LinkClosing
import com.nbtapps.store.model.PackageNeed
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.RemoveTarget
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import java.io.IOException
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The job repository against a stand-in agent that answers what each test tells it to. */
class JobRepositoryTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val kept = AtomicInteger()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val keys = MemoryPairingKeys()
    private val store by lazy { PackageStore(folder.newFolder("packages")) }

    /** The app's own reconnect after the agent restarted, with waits a test can sit out and a window it cannot run out of by being slow. */
    private val patient = Rejoin(firstAfterMs = 50, pauseMs = 50, giveUpAfterMs = 30_000)
    private val jobs by lazy {
        JobRepository(headUnits, connector, identity, keys, store, { kept.incrementAndGet() }, scope, now = { 1234L }, rejoin = patient)
    }

    private val car = HeadUnit("car", "Car", listOf(TcpEndpoint("192.168.0.1", 7144)))
    private val runtimeFile = AgentVectors.file("packages/good/org.nbtapp.runtime-2.1.0.nbtpkg")
    private val planId = "1011121314151617"
    private val runtimeRow = """{"id":"org.nbtapp.runtime","name":"NBT Runtime","version":"2.1.0","key":"${"ab".repeat(32)}","flags":[]}"""

    @After
    fun tearDown() = scope.cancel()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(car)
            store.load()
            block()
        }
    }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private fun job(state: String, seq: Long, extra: String = "") =
        """{"planId":"$planId","state":"$state","step":1,"steps":1,"id":"org.nbtapp.runtime"$extra,"seq":$seq}"""

    private fun progress(state: String, extra: String = "") =
        """{"planId":"$planId","state":"$state","step":1,"steps":1,"id":"org.nbtapp.runtime"$extra}"""

    private val done =
        """{"planId":"$planId","state":"succeeded","steps":[{"action":"install","id":"org.nbtapp.runtime","version":"2.1.0","result":"done"}],"restart":"hmi-restart"}"""

    /** An agent that plans the runtime package after fetching its manifest, and starts the job fetching. */
    private fun planning(agent: FakeAgent, stored: StoredPackage) {
        agent.handlers["job.plan"] = { params ->
            val offer = (params.getValue("offers") as JsonArray)[0] as JsonObject
            val manifest = agent.fetch(offer.getValue("manifestSha256").jsonPrimitive.content, 0)
            check(sha256Hex(manifest) == stored.manifestSha256) { "the phone served another manifest" }
            """{"planId":"$planId","steps":[{"action":"install","id":"org.nbtapp.runtime","version":"2.1.0","sha256":"${stored.sha256}"}],""" +
                """"missing":[],"problems":[],"flags":[],"fetchBytes":${stored.size},"installBytes":93,"restart":"none"}"""
        }
        agent.handlers["job.start"] = { """{"job":${job("fetching", 0, ""","phase":"fetch"""")}}""" }
    }

    private suspend fun stored(): StoredPackage = runtimeFile.inputStream().use { store.add(it, listOf(AgentVectors.packageKey)) }

    private suspend fun startedJob(stored: StoredPackage, jobs: JobRepository = this.jobs): Plan {
        assertTrue(jobs.connect("car").isSuccess)
        val plan = jobs.plan(listOf(InstallTarget("org.nbtapp.runtime", "2.1.0")), emptyList(), listOf(stored)).getOrThrow()
        jobs.start(plan).getOrThrow()
        return plan
    }

    @Test
    fun `connecting says hello, reads the profile and keeps it with the head unit`() = test {
        connector.onConnect = { it.installed = "[$runtimeRow]" }

        assertTrue(jobs.connect("car").isSuccess)

        val connected = jobs.connection.value as ConnectionState.Connected
        assertEquals("car", connected.headUnitId)
        assertEquals("1.0.0", connected.info.agentVersion)
        assertEquals("org.nbtapp.runtime", connected.installed.single().id)
        val hello = connector.agent.requestsOf("hello").single()
        assertEquals("5f2c9a1e07b34d6688a0c1d2e3f40516", hello.getValue("phoneId").jsonPrimitive.content)
        assertEquals("phone", hello.getValue("role").jsonPrimitive.content)

        val saved = requireNotNull(headUnits.headUnit("car").first())
        assertEquals("000102030405060708090a0b0c0d0e0f", saved.headUnitId)
        assertEquals("1.0.0", saved.profile?.agentVersion)
        assertEquals(4000L, saved.profile?.free?.staging)
        assertEquals(listOf("org.nbtapp.runtime"), saved.profile?.installed?.map { it.id })
        assertEquals(1234L, saved.profile?.seenAt)
        assertNull(jobs.job.value)

        jobs.disconnect()
        assertEquals(ConnectionState.Disconnected, jobs.connection.value)
        // What the agent reported stays with the head unit for when the phone is not in the car.
        assertEquals("1.0.0", headUnits.headUnit("car").first()?.profile?.agentVersion)
    }

    @Test
    fun `an agent with another id than the one saved is not this head unit`() = test {
        headUnits.update("car") { it.copy(headUnitId = "ff".repeat(16)) }

        val result = jobs.connect("car")

        assertTrue(result.isFailure)
        val failed = jobs.connection.value as ConnectionState.Failed
        assertTrue(failed.reason, failed.reason.contains("Car"))
        assertEquals("ff".repeat(16), headUnits.headUnit("car").first()?.headUnitId)
        assertTrue(jobs.plan(emptyList(), listOf(RemoveTarget("org.nbtapp.runtime")), emptyList()).isFailure)
    }

    @Test
    fun `a head unit that can't be reached, or that this build has no link for, fails with the reason`() = test {
        connector.unreachable = "No route to the car"
        assertTrue(jobs.connect("car").isFailure)
        assertEquals(ConnectionState.Failed("car", "No route to the car"), jobs.connection.value)

        headUnits.add(HeadUnit("bt", "Bluetooth car", listOf(SerialEndpoint("00:11:22:33:44:55", "BMW"))))
        assertTrue(jobs.connect("bt").isFailure)
        assertTrue((jobs.connection.value as ConnectionState.Failed).reason.contains("no way"))
        assertTrue(jobs.connect("nobody").isFailure)
    }

    @Test
    fun `a head unit with a Bluetooth serial endpoint is reached over it, with the smaller frame offered`() = test {
        val serial = FakeConnector(serial = true)
        val overSerial = JobRepository(headUnits, serial, identity, keys, store, { kept.incrementAndGet() }, scope, now = { 1234L })
        val endpoint = SerialEndpoint("00:11:22:33:44:55", "BMW")
        // The TCP endpoint is first, and this connector has no link for it.
        headUnits.add(HeadUnit("bt", "Bluetooth car", listOf(TcpEndpoint("192.168.0.1", 7144), endpoint)))

        assertTrue(overSerial.connect("bt").isSuccess)
        assertEquals(endpoint, (overSerial.connection.value as ConnectionState.Connected).endpoint)
        assertEquals("4096", serial.agent.requestsOf("hello").single().getValue("maxFrame").toString())
        // And 65,536 where the link is not serial.
        assertTrue(jobs.connect("car").isSuccess)
        assertEquals("65536", connector.agent.requestsOf("hello").single().getValue("maxFrame").toString())
    }

    private val bluetoothCar = HeadUnit("bt", "Bluetooth car", listOf(SerialEndpoint("00:11:22:33:44:55", "BMW")))

    /** A repository whose waits for `hello` are short enough for a test to sit them out. */
    private fun impatient(connector: LinkConnector) = JobRepository(
        headUnits, connector, identity, keys, store, { kept.incrementAndGet() }, scope, now = { 1234L },
        greeting = Greeting(timeoutMs = 3_000, retryTimeoutMs = 300, pauseMs = 10),
    )

    @Test
    fun `a serial link that ends before hello is answered is opened again, and the one that ended is closed`() = test {
        headUnits.add(bluetoothCar)
        val serial = FakeConnector(serial = true)
        val counting = Counting(serial)
        // The car gave the first link to another program, or the agent let it go: it ends with nothing said.
        serial.onConnect = { agent -> if (serial.agents.isEmpty()) agent.drop() }
        val jobs = impatient(counting)

        assertTrue(jobs.connect("bt").isSuccess)

        assertTrue(jobs.connection.value is ConnectionState.Connected)
        assertEquals(2, serial.agents.size)
        assertEquals(2, counting.opened.get())
        assertEquals("the link that ended stayed open", 1, counting.closed.get())
        assertEquals(1, serial.agent.requestsOf("hello").size)
    }

    @Test
    fun `a serial link on which hello gets no answer is closed and opened again`() = test {
        headUnits.add(bluetoothCar)
        val serial = FakeConnector(serial = true)
        val counting = Counting(serial)
        val release = CountDownLatch(1)
        // Nobody reads the first link to its end: it stays up and silent.
        serial.onConnect = { agent ->
            if (serial.agents.isEmpty()) {
                agent.handlers["hello"] = {
                    release.await(30, TimeUnit.SECONDS)
                    throw Refusal("internal")
                }
            }
        }
        try {
            assertTrue(impatient(counting).connect("bt").isSuccess)

            assertEquals(2, serial.agents.size)
            assertEquals("the silent link stayed open", 1, counting.closed.get())
        } finally {
            release.countDown()
        }
    }

    @Test
    fun `after three serial links with no answer the connect fails, and says how many it opened`() = test {
        headUnits.add(bluetoothCar)
        val serial = FakeConnector(serial = true)
        val counting = Counting(serial)
        serial.onConnect = { it.drop() }
        val jobs = impatient(counting)

        assertTrue(jobs.connect("bt").isFailure)

        val reason = (jobs.connection.value as ConnectionState.Failed).reason
        assertTrue(reason, reason.endsWith(" (3 links were tried)"))
        assertEquals(3, serial.agents.size)
        assertEquals("a link stayed open", 3, counting.closed.get())
    }

    @Test
    fun `an answer that refuses is the agent's, and a link that is not serial is opened once`() = test {
        headUnits.add(bluetoothCar)
        val serial = FakeConnector(serial = true)
        serial.onConnect = { it.handlers["hello"] = { throw Refusal("unsupported-version") } }
        assertTrue(impatient(serial).connect("bt").isFailure)
        assertEquals(1, serial.agents.size)

        // Over TCP nothing else can have taken the link, so one that ends is not tried again.
        connector.onConnect = { it.drop() }
        val jobs = impatient(connector)
        assertTrue(jobs.connect("car").isFailure)
        assertEquals(1, connector.agents.size)
        assertFalse((jobs.connection.value as ConnectionState.Failed).reason.contains("were tried"))
    }

    // Pairing and `auth`, against an agent that asks for them as the real one does on Bluetooth serial.

    private val phoneId = "5f2c9a1e07b34d6688a0c1d2e3f40516"
    private val agentId = "000102030405060708090a0b0c0d0e0f"
    private val overTcp = car.endpoints.single()

    /** Makes the connector's agents ask for `auth`, and returns what they know of phones. */
    private fun guarded(): FakePhones = FakePhones().also { connector.phones = it }

    private fun methods(agent: FakeAgent) = agent.requests.map { it.first }

    @Test
    fun `a phone that holds the agent's key proves it and is connected`() = test {
        val phones = guarded()
        keys.put(agentId, phones.pair(phoneId))

        assertTrue(jobs.connect("car").isSuccess)

        assertTrue(jobs.connection.value is ConnectionState.Connected)
        assertTrue(connector.agent.trusted)
        // Both halves of `auth` come before anything is asked.
        assertEquals(listOf("hello", "auth", "auth", "info.get"), methods(connector.agent).take(4))
        assertEquals(agentId, headUnits.headUnit("car").first()?.headUnitId)
        assertEquals(0, phones.issued.get())
    }

    @Test
    fun `a phone with no key is held at needs pairing, and the code from the car's screen pairs it`() = test {
        val phones = guarded()

        assertTrue(jobs.connect("car").exceptionOrNull() is PairingNeeded)

        assertEquals(ConnectionState.NeedsPairing("car", overTcp, stale = false), jobs.connection.value)
        // The link is held for the code. Nothing can be asked over it, and the id the agent gave is not kept yet.
        assertTrue(jobs.refresh().isFailure)
        assertTrue(jobs.plan(emptyList(), listOf(RemoveTarget("org.nbtapp.runtime")), emptyList()).isFailure)
        assertNull(headUnits.headUnit("car").first()?.headUnitId)

        // With no code on the car's screen, and with a wrong one, the link goes on waiting.
        val closed = jobs.pair("123456").exceptionOrNull() as AgentRefusal
        assertEquals("forbidden", closed.code)
        assertEquals(AgentText.pairing("forbidden"), closed.message)
        phones.code = "951120"
        val wrong = jobs.pair("951121").exceptionOrNull() as AgentRefusal
        assertEquals("rejected", wrong.code)
        assertEquals(AgentText.pairing("rejected"), wrong.message)
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
        assertEquals(emptySet<String>(), keys.headUnitIds)

        assertTrue(jobs.pair("951120").isSuccess)

        // Paired and proved over the same link, which is now as any connected one.
        assertEquals("car", (jobs.connection.value as ConnectionState.Connected).headUnitId)
        assertEquals(1, connector.agents.size)
        assertTrue(connector.agent.trusted)
        assertArrayEquals(phones.keys[phoneId], keys.key(agentId))
        assertEquals(agentId, headUnits.headUnit("car").first()?.headUnitId)
        assertTrue(jobs.refresh().isSuccess)
        assertTrue(jobs.pair("951120").isFailure)

        // The next link needs no code.
        jobs.disconnect()
        assertTrue(jobs.connect("car").isSuccess)
        assertEquals(listOf("hello", "auth", "auth", "info.get"), methods(connector.agent).take(4))
        assertEquals(1, phones.issued.get())
    }

    @Test
    fun `a key the agent no longer takes is dropped, and the phone is paired anew over a second link`() = test {
        val phones = guarded()
        keys.put(agentId, phones.pair(phoneId))
        // "Forget all phones" on the car's screen.
        phones.forgetAll()

        assertTrue(jobs.connect("car").exceptionOrNull() is PairingNeeded)

        assertEquals(ConnectionState.NeedsPairing("car", overTcp, stale = true), jobs.connection.value)
        // The agent ended the link on which `auth` failed. The second is held, and no key was tried on it.
        assertEquals(2, connector.agents.size)
        assertEquals(listOf("hello", "auth"), methods(connector.agents.first()))
        assertEquals(listOf("hello", "info.get"), methods(connector.agent))
        assertEquals(emptySet<String>(), keys.headUnitIds)

        phones.code = "000042"
        assertTrue(jobs.pair("000042").isSuccess)
        assertTrue(jobs.connection.value is ConnectionState.Connected)
        assertArrayEquals(phones.keys[phoneId], keys.key(agentId))
    }

    @Test
    fun `an agent that proves itself with another key is not believed, and the phone's key is dropped`() = test {
        val phones = guarded()
        keys.put(agentId, phones.pair(phoneId))
        phones.provesWrong = true

        assertTrue(jobs.connect("car").exceptionOrNull() is PairingNeeded)

        // The phone's own proof never went: the first link saw its nonce and no more.
        assertEquals(listOf("hello", "auth"), methods(connector.agents.first()))
        assertFalse(connector.agents.first().trusted)
        assertEquals(ConnectionState.NeedsPairing("car", overTcp, stale = true), jobs.connection.value)
        assertEquals(emptySet<String>(), keys.headUnitIds)
    }

    @Test
    fun `an agent that asks for no auth is used as it is, and a key the phone holds for it is kept`() = test {
        // What every other test here connects to: an agent from before there was `auth`, which has no such method.
        keys.put(agentId, ByteArray(32) { 1 })

        assertTrue(jobs.connect("car").isSuccess)

        assertEquals(listOf("hello", "auth", "info.get"), methods(connector.agent).take(3))
        assertEquals(setOf(agentId), keys.headUnitIds)
    }

    @Test
    fun `a link that ends while it waits to be paired shows as lost, and one that is let go is closed`() = test {
        guarded()
        val counting = Counting(connector)
        val jobs = JobRepository(headUnits, counting, identity, keys, store, { kept.incrementAndGet() }, scope, now = { 1234L })
        assertTrue(jobs.connect("car").isFailure)
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)

        // The car goes out of range.
        connector.agent.drop()
        await(jobs.connection, "the lost link") { it is ConnectionState.Failed }
        assertTrue(jobs.pair("123456").isFailure)

        assertTrue(jobs.connect("car").isFailure)
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
        jobs.disconnect()
        assertEquals(ConnectionState.Disconnected, jobs.connection.value)
        assertEquals("a link stayed open", counting.opened.get(), counting.closed.get())

        // A connect in its place lets the waiting one go too.
        assertTrue(jobs.connect("car").isFailure)
        assertTrue(jobs.connect("car").isFailure)
        assertEquals("the link before it stayed open", counting.opened.get() - 1, counting.closed.get())
    }

    @Test
    fun `a key that cannot be kept fails the pairing with the reason, and the link goes on waiting`() = test {
        val phones = guarded()
        assertTrue(jobs.connect("car").isFailure)
        phones.code = "111111"
        keys.broken = "The phone's key store failed: no room"

        val failure = jobs.pair("111111").exceptionOrNull()

        assertEquals("The phone's key store failed: no room", failure?.message)
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
        assertFalse(connector.agent.trusted)
    }

    /** Reaches every endpoint, lets none through, and keeps the order they were tried in. */
    private class Unreachable : LinkConnector {
        val tried = CopyOnWriteArrayList<Endpoint>()
        override fun supports(endpoint: Endpoint) = true
        override suspend fun connect(endpoint: Endpoint): Link {
            tried += endpoint
            throw IOException(if (endpoint is SerialEndpoint) "the car didn't take the link" else "no route to ${(endpoint as TcpEndpoint).host}")
        }
    }

    /** Hands on what [inner] opens, and counts the links that were opened and the ones closed again. */
    private class Counting(private val inner: LinkConnector) : LinkConnector {
        val opened = AtomicInteger()
        val closed = AtomicInteger()

        /** When set, a connect waits here before it goes to [inner]: a link that takes its time to come up. */
        @Volatile
        var hold: CompletableDeferred<Unit>? = null
        val waiting = CompletableDeferred<Unit>()

        override fun supports(endpoint: Endpoint) = inner.supports(endpoint)
        override suspend fun connect(endpoint: Endpoint): Link {
            hold?.let {
                waiting.complete(Unit)
                it.await()
            }
            val link = inner.connect(endpoint)
            opened.incrementAndGet()
            return object : Link by link {
                private val once = AtomicBoolean()
                override fun close() {
                    if (once.compareAndSet(false, true)) closed.incrementAndGet()
                    link.close()
                }
            }
        }
    }

    private fun repository(connector: LinkConnector, rejoin: Rejoin = patient) =
        JobRepository(headUnits, connector, identity, keys, store, { kept.incrementAndGet() }, scope, now = { 1234L }, rejoin = rejoin)

    @Test
    fun `the TCP endpoints are tried before Bluetooth serial, whatever order they were saved in`() = test {
        val bluetooth = SerialEndpoint("00:11:22:33:44:55", "BMW")
        val firstTcp = TcpEndpoint("192.168.0.1", 7144)
        val secondTcp = TcpEndpoint("10.0.0.1", 7144)
        headUnits.add(HeadUnit("both", "Car", listOf(bluetooth, firstTcp, secondTcp)))
        val unreachable = Unreachable()

        assertTrue(repository(unreachable).connect("both").isFailure)
        assertEquals(listOf(firstTcp, secondTcp, bluetooth), unreachable.tried)

        // And where both would work, the link is the TCP one: the serial link is not opened at all.
        val overTcp = FakeConnector()
        val overSerial = FakeConnector(serial = true)
        val either = repository(LinkConnectors(listOf(overSerial, overTcp)))
        assertTrue(either.connect("both").isSuccess)
        assertEquals(firstTcp, (either.connection.value as ConnectionState.Connected).endpoint)
        assertTrue(overSerial.agents.isEmpty())
    }

    @Test
    fun `when several ways were tried, the reason names each with why it failed`() = test {
        headUnits.add(
            HeadUnit("both", "Car", listOf(SerialEndpoint("00:11:22:33:44:55", "BMW"), TcpEndpoint("192.168.0.1", 7144))),
        )
        val jobs = repository(Unreachable())

        val result = jobs.connect("both")

        val reason = "192.168.0.1:7144: no route to 192.168.0.1\nBluetooth serial to BMW: the car didn't take the link"
        assertEquals(ConnectionState.Failed("both", reason), jobs.connection.value)
        assertEquals(reason, result.exceptionOrNull()?.message)
        // One way alone needs no name.
        assertTrue(jobs.connect("car").isFailure)
        assertEquals(ConnectionState.Failed("car", "no route to 192.168.0.1"), jobs.connection.value)
    }

    @Test
    fun `a link that came up to an agent the app then refuses is closed again`() = test {
        // The agent answers hello, but it is not the head unit that was saved.
        headUnits.update("car") { it.copy(headUnitId = "ff".repeat(16)) }
        val counting = Counting(connector)

        assertTrue(repository(counting).connect("car").isFailure)

        assertEquals(1, counting.opened.get())
        assertEquals("the link stayed open", 1, counting.closed.get())
    }

    @Test
    fun `a connect whose caller is cancelled before the link is up leaves no connecting state behind`() = test {
        val counting = Counting(connector).apply { hold = CompletableDeferred() }
        val jobs = repository(counting)

        // As a screen's scope does when the user leaves it.
        val caller = launch { jobs.connect("car") }
        counting.waiting.await()
        assertEquals(ConnectionState.Connecting("car"), jobs.connection.value)
        caller.cancelAndJoin()

        assertEquals(ConnectionState.Disconnected, jobs.connection.value)
        assertEquals(0, counting.opened.get())
        // And the next one goes through.
        counting.hold = null
        assertTrue(jobs.connect("car").isSuccess)
        assertTrue(jobs.connection.value is ConnectionState.Connected)
    }

    @Test
    fun `a connect whose caller is cancelled while the agent has not yet answered closes the link it opened`() = test {
        val counting = Counting(connector)
        val jobs = repository(counting)
        val asked = CountDownLatch(1)
        val release = CountDownLatch(1)
        connector.onConnect = { agent ->
            val hello = agent.handlers.getValue("hello")
            agent.handlers["hello"] = { params ->
                asked.countDown()
                release.await(30, TimeUnit.SECONDS)
                hello(params)
            }
        }
        try {
            val caller = launch { jobs.connect("car") }
            assertTrue("hello never reached the agent", asked.await(10, TimeUnit.SECONDS))
            caller.cancelAndJoin()

            assertEquals(ConnectionState.Disconnected, jobs.connection.value)
            assertEquals(1, counting.opened.get())
            assertEquals("the link stayed open", 1, counting.closed.get())
            assertTrue(jobs.restart("hmi").isFailure)
        } finally {
            release.countDown()
        }
    }

    @Test
    fun `a job runs from the plan to its result, with the phone serving what the agent fetches`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }

        val plan = startedJob(stored)
        val agent = connector.agent

        assertEquals(planId, plan.planId)
        val request = agent.requestsOf("job.plan").single()
        assertEquals("""[{"id":"org.nbtapp.runtime","version":"2.1.0"}]""", request["install"].toString())
        assertEquals(stored.sha256, ((request.getValue("offers") as JsonArray)[0] as JsonObject).getValue("sha256").jsonPrimitive.content)
        assertEquals(1, kept.get())
        val started = requireNotNull(jobs.job.value)
        assertEquals(JobState.FETCHING, started.job.state)
        assertEquals(plan, started.plan)
        assertEquals("car", started.headUnitId)

        // The agent drives: it pulls the archive in ranges and says how far it is.
        val bytes = agent.fetch(stored.sha256, 0, 500) + agent.fetch(stored.sha256, 500)
        assertArrayEquals(runtimeFile.readBytes(), bytes)
        agent.event("job.progress", 1, progress("fetching", ""","phase":"fetch","bytes":956,"total":956"""))
        assertEquals(956L, await(jobs.job, "the fetch progress") { it?.job?.bytes == 956L }?.job?.total)
        agent.event("job.progress", 2, progress("installing", ""","phase":"files""""))
        agent.event("job.log", 3, """{"planId":"$planId","step":1,"source":"postInstall","text":"cache built\n"}""")
        agent.installed = "[$runtimeRow]"
        agent.restartNeeded = "hmi-restart"
        agent.event("job.done", 4, done)

        val ended = requireNotNull(await(jobs.job, "the job's end") { it?.ended == true })
        assertEquals(JobState.SUCCEEDED, ended.job.state)
        assertEquals("done", ended.job.result?.steps?.single()?.result)
        assertEquals("hmi-restart", ended.job.result?.restart)
        assertEquals(listOf("postInstall" to "cache built\n"), ended.log.map { it.source to it.text })
        assertEquals(4L, ended.job.seq)

        // The installed list and the profile are read again once the job is done.
        val connected = await(jobs.connection, "the new installed list") { (it as? ConnectionState.Connected)?.installed?.size == 1 }
        assertEquals("hmi-restart", (connected as ConnectionState.Connected).info.restartNeeded)
        // The profile is saved just after the connection shows it.
        val saved = withTimeoutOrNull(10_000) { headUnits.headUnit("car").first { it?.profile?.restartNeeded == "hmi-restart" } }
        assertEquals(1, saved?.profile?.installed?.size)
    }

    @Test
    fun `events already seen are not applied twice, and an older one never overwrites a newer`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        startedJob(stored)
        val agent = connector.agent

        agent.event("job.progress", 5, progress("installing", ""","phase":"files""""))
        await(jobs.job, "phase files") { it?.job?.phase == "files" }
        agent.event("job.progress", 4, progress("installing", ""","phase":"backup""""))
        agent.event("job.log", 5, """{"planId":"$planId","step":1,"source":"agent","text":"again\n"}""")
        agent.event("job.log", 6, """{"planId":"$planId","step":1,"source":"agent","text":"once\n"}""")

        val view = await(jobs.job, "the log line") { it?.log?.isNotEmpty() == true }
        assertEquals("files", view?.job?.phase)
        assertEquals(listOf("once\n"), view?.log?.map { it.text })
    }

    @Test
    fun `a job whose link drops while fetching goes on over the next link`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        startedJob(stored)
        connector.agent.event("job.progress", 1, progress("fetching", ""","phase":"fetch","bytes":100,"total":956"""))
        await(jobs.job, "the fetch progress") { it?.job?.bytes == 100L }

        connector.agent.drop()

        await(jobs.connection, "the lost link") { it is ConnectionState.Failed }
        assertTrue(requireNotNull(jobs.job.value).linkLost)
        assertTrue(jobs.cancel().isFailure)

        // The head unit kept the job, paused. The app names its plan again, and serves it over the new link.
        connector.onConnect = { agent ->
            agent.handlers["job.status"] = { """{"job":${job("paused", 5, ""","phase":"fetch","bytes":100,"total":956""")},"events":[],"more":false}""" }
            agent.handlers["job.start"] = { """{"job":${job("fetching", 5, ""","phase":"fetch","bytes":100,"total":956""")}}""" }
        }
        assertTrue(jobs.connect("car").isSuccess)

        val resumed = requireNotNull(jobs.job.value)
        assertEquals(JobState.FETCHING, resumed.job.state)
        assertFalse(resumed.linkLost)
        assertEquals(listOf(planId), connector.agent.requestsOf("job.start").map { it.getValue("planId").jsonPrimitive.content })
        assertEquals(2, kept.get())
        assertEquals(2, connector.agents.size)
        assertArrayEquals(runtimeFile.readBytes().copyOfRange(100, 956), connector.agent.fetch(stored.sha256, 100))
    }

    @Test
    fun `a job the head unit no longer has is shown as gone`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        startedJob(stored)
        connector.agent.drop()
        await(jobs.connection, "the lost link") { it is ConnectionState.Failed }

        // The head unit lost power: its agent starts with no job on record.
        connector.onConnect = {}
        assertTrue(jobs.connect("car").isSuccess)

        val view = requireNotNull(jobs.job.value)
        assertTrue(view.forgotten)
        assertFalse(view.linkLost)
        assertTrue(connector.agent.requestsOf("job.start").isEmpty())
    }

    @Test
    fun `a job found running on connecting is followed, with the log it wrote before`() = test {
        connector.onConnect = { agent ->
            agent.handlers["job.status"] = { params ->
                val events = if ("after" in params) {
                    """[{"event":"job.log","seq":3,"params":{"planId":"$planId","step":1,"source":"agent","text":"from the stick\n"}}]"""
                } else {
                    "[]"
                }
                """{"job":${job("installing", 4, ""","phase":"files"""")},"events":$events,"more":false}"""
            }
        }

        assertTrue(jobs.connect("car").isSuccess)

        val found = requireNotNull(jobs.job.value)
        assertEquals(planId, found.planId)
        assertNull("this app did not plan it", found.plan)
        assertEquals("files", found.job.phase)
        assertEquals(listOf("from the stick\n"), found.log.map { it.text })
        val status = connector.agent.requestsOf("job.status")
        assertEquals("{}", status[0].toString())
        assertEquals("""{"planId":"$planId","after":0}""", status[1].toString())
        assertEquals(0, kept.get())

        // Its events arrive on this link as on every other.
        connector.agent.event("job.progress", 4, progress("installing", ""","phase":"backup""""))
        connector.agent.event("job.progress", 5, progress("installing", ""","phase":"post-install""""))
        assertEquals("post-install", await(jobs.job, "the next phase") { it?.job?.seq == 5L }?.job?.phase)
        connector.agent.event("job.done", 6, done)
        assertEquals(JobState.SUCCEEDED, await(jobs.job, "the end") { it?.ended == true }?.job?.state)
    }

    @Test
    fun `stopping sends job-cancel, and a refusal comes back in the app's own words`() = test {
        val stored = stored()
        connector.onConnect = { agent ->
            planning(agent, stored)
            agent.handlers["job.cancel"] = { "{}" }
        }
        startedJob(stored)

        assertTrue(jobs.cancel().isSuccess)
        assertEquals(planId, connector.agent.requestsOf("job.cancel").single().getValue("planId").jsonPrimitive.content)

        connector.agent.handlers["job.cancel"] = { throw Refusal("too-late") }
        val refusal = jobs.cancel().exceptionOrNull() as AgentRefusal
        assertEquals("too-late", refusal.code)
        assertEquals(AgentText.request("too-late"), refusal.message)
    }

    @Test
    fun `a plan the agent will not start leaves no job behind`() = test {
        val stored = stored()
        connector.onConnect = { agent ->
            planning(agent, stored)
            agent.handlers["job.start"] = { throw Refusal("plan-stale") }
        }
        assertTrue(jobs.connect("car").isSuccess)
        val plan = jobs.plan(listOf(InstallTarget("org.nbtapp.runtime", "2.1.0")), emptyList(), listOf(stored)).getOrThrow()

        val refusal = jobs.start(plan).exceptionOrNull() as AgentRefusal

        assertEquals("plan-stale", refusal.code)
        assertNull(jobs.job.value)
        assertEquals(0, kept.get())
    }

    @Test
    fun `restart is passed on, and nothing can be asked without a link`() = test {
        assertTrue(jobs.restart("hmi").isFailure)
        assertTrue(jobs.refresh().isFailure)

        connector.onConnect = { it.handlers["system.restart"] = { "{}" } }
        assertTrue(jobs.connect("car").isSuccess)
        assertTrue(jobs.restart("hmi").isSuccess)
        assertEquals("""{"kind":"hmi"}""", connector.agent.requestsOf("system.restart").single().toString())
    }

    // How a link ends: the agent says `link.closing` before it ends one of its own accord, and the app ends it.

    @Test
    fun `an agent that says why it ends the link has the link ended by the phone, and the reason is shown`() = test {
        for (reason in listOf(LinkClosing.STOPPING, LinkClosing.SILENT, LinkClosing.REPLACED, "moving-house")) {
            assertTrue(jobs.connect("car").isSuccess)
            val agent = connector.agent
            val asked = agent.requests.size

            agent.closing(reason)

            assertEquals(
                ConnectionState.Failed("car", AgentText.closing(reason)),
                await(jobs.connection, "the end for $reason") { it is ConnectionState.Failed },
            )
            assertTrue("$reason: the phone did not end the link", agent.phoneLeft())
            assertEquals("$reason: the phone asked something after link.closing", asked, agent.requests.size)
        }
        // None of these is followed by a connect of the app's own.
        delay(300)
        assertEquals(4, connector.agents.size)
        assertTrue(jobs.connection.value is ConnectionState.Failed)
    }

    @Test
    fun `a job whose agent says it stops is one whose link was lost`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        startedJob(stored)

        connector.agent.closing(LinkClosing.STOPPING)

        await(jobs.connection, "the end") { it is ConnectionState.Failed }
        val view = requireNotNull(jobs.job.value)
        assertTrue(view.linkLost)
        assertFalse(view.reconnecting)
    }

    @Test
    fun `an agent that says it forgot the phone has its key dropped, and the next link goes straight to pairing`() = test {
        val phones = guarded()
        keys.put(agentId, phones.pair(phoneId))
        assertTrue(jobs.connect("car").isSuccess)

        // "Forget all phones" on the car's screen, with the app connected.
        phones.forgetAll()
        connector.agent.closing(LinkClosing.FORGOTTEN)

        assertEquals(
            ConnectionState.Failed("car", AgentText.closing(LinkClosing.FORGOTTEN)),
            await(jobs.connection, "the end") { it is ConnectionState.Failed },
        )
        assertTrue(connector.agent.phoneLeft())
        assertEquals(emptySet<String>(), keys.headUnitIds)

        // One link, and no key tried on it: not one for `auth` to fail on and then another.
        assertTrue(jobs.connect("car").exceptionOrNull() is PairingNeeded)
        assertEquals(2, connector.agents.size)
        assertEquals(listOf("hello", "info.get"), methods(connector.agent))
        assertEquals(ConnectionState.NeedsPairing("car", overTcp, stale = true), jobs.connection.value)

        phones.code = "000042"
        assertTrue(jobs.pair("000042").isSuccess)
        assertTrue(jobs.connection.value is ConnectionState.Connected)

        // Paired again, the head unit is no longer one that forgot this phone: a key that goes missing on the
        // phone's own side later is not put down to the car.
        jobs.disconnect()
        keys.remove(agentId)
        assertTrue(jobs.connect("car").exceptionOrNull() is PairingNeeded)
        assertEquals(ConnectionState.NeedsPairing("car", overTcp, stale = false), jobs.connection.value)
    }

    // ---- starting with the head unit ----

    private val pluginRow = """{"id":"org.nbtapp.plugin","name":"NBTApp iDrive plugin","version":"1.0.0","key":"${"ab".repeat(32)}","flags":[]}"""

    /**
     * Makes [agent] one that has the switch, as the real one has it: it lists the capability, says how things
     * stand by what is in [state], and takes a switch only where the iDrive plugin is among what is installed.
     */
    private fun withSwitch(agent: FakeAgent, state: AtomicReference<String>) {
        agent.handlers["hello"] = {
            """{"protocolMin":1,"protocolMax":1,"role":"agent","headUnitId":"$agentId","agentVersion":"1.0.0","maxFrame":65536,"capabilities":["autostart","one from later"]}"""
        }
        fun answer(on: Boolean): String = if (agent.installed.contains("org.nbtapp.plugin")) {
            """{"on":$on,"available":true,"starts":0}"""
        } else {
            """{"on":$on,"available":false,"reason":"plugin-missing","needs":{"id":"org.nbtapp.plugin","version":">=1.0.0"},"starts":0}"""
        }
        agent.handlers["autostart.get"] = { answer(state.get() == "on") }
        agent.handlers["autostart.set"] = { params ->
            val on = params.getValue("on").jsonPrimitive.content == "true"
            if (on && !agent.installed.contains("org.nbtapp.plugin")) throw Refusal("plugin-missing")
            state.set(if (on) "on" else "off")
            answer(on)
        }
    }

    private fun autostart() = (jobs.connection.value as ConnectionState.Connected).autostart

    @Test
    fun `an agent that lists no such capability is asked nothing about starting with the car`() = test {
        assertTrue(jobs.connect("car").isSuccess)

        assertNull(autostart())
        assertEquals(listOf("hello", "info.get", "packages.list", "job.status"), methods(connector.agent))
        // And where a screen offered the switch all the same, the agent's refusal is all that comes of it.
        val refused = jobs.setAutostart(true).exceptionOrNull() as AgentRefusal
        assertEquals("unsupported", refused.code)
        assertNull(autostart())
    }

    @Test
    fun `an agent with the switch says how things stand, and what it lacks to be started with the car`() = test {
        val state = AtomicReference("off")
        connector.onConnect = { withSwitch(it, state) }

        assertTrue(jobs.connect("car").isSuccess)

        assertEquals(
            Autostart(on = false, available = false, reason = AutostartReason.PLUGIN_MISSING, needs = PackageNeed("org.nbtapp.plugin", ">=1.0.0")),
            autostart(),
        )
        assertEquals(listOf("hello", "info.get", "packages.list", "autostart.get", "job.status"), methods(connector.agent))

        // Switching it on is refused, in the app's words, and what the agent says now is what is shown.
        val refused = jobs.setAutostart(true).exceptionOrNull() as AgentRefusal
        assertEquals(AutostartReason.PLUGIN_MISSING, refused.code)
        assertEquals(AgentText.autostartReason(AutostartReason.PLUGIN_MISSING), refused.message)
        assertEquals(false, autostart()?.on)
        assertEquals("off", state.get())
    }

    @Test
    fun `the switch is thrown over the link, and a job that installs the plugin makes it one that can be`() = test {
        val state = AtomicReference("off")
        val stored = stored()
        connector.onConnect = {
            withSwitch(it, state)
            planning(it, stored)
        }
        startedJob(stored)
        assertEquals(false, autostart()?.available)

        // The job that ends is what changes the head unit: here it brought the plugin.
        connector.agent.installed = "[$pluginRow]"
        connector.agent.event("job.done", 1, done)
        await(jobs.connection, "the switch to be had") { (it as? ConnectionState.Connected)?.autostart?.available == true }
        assertEquals(Autostart(on = false, available = true), autostart())

        assertTrue(jobs.setAutostart(true).isSuccess)
        assertEquals(Autostart(on = true, available = true), autostart())
        assertEquals("on", state.get())
        assertEquals("""{"on":true}""", connector.agent.requestsOf("autostart.set").single().toString())

        assertTrue(jobs.setAutostart(false).isSuccess)
        assertEquals(Autostart(on = false, available = true), autostart())
        assertEquals("off", state.get())

        // With no link there is nothing to throw.
        jobs.disconnect()
        assertTrue(jobs.setAutostart(true).isFailure)
        assertEquals("off", state.get())
    }

    @Test
    fun `an agent that lists the capability and will not say is one without the switch, and the link stands`() = test {
        connector.onConnect = { agent ->
            withSwitch(agent, AtomicReference("off"))
            agent.handlers.remove("autostart.get")
        }

        assertTrue(jobs.connect("car").isSuccess)

        assertNull(autostart())
        assertTrue(jobs.connection.value is ConnectionState.Connected)
    }

    @Test
    fun `an answer that is not the switch's ends in a failure the user is shown, not in a switch`() = test {
        connector.onConnect = { agent ->
            withSwitch(agent, AtomicReference("off"))
            agent.installed = "[$pluginRow]"
            agent.handlers["autostart.set"] = { """{"available":true}""" }
        }
        assertTrue(jobs.connect("car").isSuccess)

        val failure = jobs.setAutostart(true).exceptionOrNull()

        assertEquals("The head unit sent an answer this app can't read", failure?.message)
        assertEquals(Autostart(on = false, available = true), autostart())
    }

    /** Makes [agent] the agent's next version, by what `info.get` says. */
    private fun newVersion(agent: FakeAgent) {
        agent.handlers["info.get"] = {
            """{"agentVersion":"1.1.0","headUnitId":"$agentId","manifestVersion":1,""" +
                """"free":{"app":1000,"data":2000,"plugins":3000,"staging":4000},"restartNeeded":"none"}"""
        }
    }

    @Test
    fun `an agent that says it restarts is connected to again by the app itself`() = test {
        assertTrue(jobs.connect("car").isSuccess)
        val old = connector.agent
        // The new version is not there at once.
        connector.unreachable = "Nothing listens there yet"

        old.closing(LinkClosing.RESTARTING)

        assertTrue(old.phoneLeft())
        // Not a failure, and nothing for the user to do. Several tries fail meanwhile, and none of them shows.
        assertEquals(ConnectionState.Connecting("car"), await(jobs.connection, "the wait") { it !is ConnectionState.Connected })
        delay(300)
        assertEquals(ConnectionState.Connecting("car"), jobs.connection.value)

        connector.onConnect = ::newVersion
        connector.unreachable = null
        val back = await(jobs.connection, "the new agent") { it is ConnectionState.Connected } as ConnectionState.Connected
        assertEquals("1.1.0", back.info.agentVersion)
        assertEquals(2, connector.agents.size)
    }

    @Test
    fun `a job that reaches restarting ends the link without being told to, and is followed over the next one`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        startedJob(stored)
        val old = connector.agent
        val asked = old.requests.size
        connector.unreachable = "Nothing listens there yet"

        // An agent from before there was `link.closing`: its job says that it restarts, and it says no more.
        old.event("job.progress", 1, progress("installing", ""","phase":"record""""))
        old.event("job.progress", 2, progress("restarting"))

        assertTrue("the phone held the link", old.phoneLeft())
        assertEquals(asked, old.requests.size)
        val waiting = requireNotNull(await(jobs.job, "the wait") { it?.reconnecting == true })
        assertEquals(JobState.RESTARTING, waiting.job.state)
        assertFalse("the service that keeps the app alive for the job would stop", waiting.linkLost)
        await(jobs.connection, "the wait") { it == ConnectionState.Connecting("car") }

        // The new version finished the job as it started.
        connector.onConnect = { agent ->
            newVersion(agent)
            agent.handlers["job.status"] = { """{"job":${job("succeeded", 3, ""","result":$done""")},"events":[],"more":false}""" }
        }
        connector.unreachable = null

        val ended = requireNotNull(await(jobs.job, "the job's end") { it?.ended == true })
        assertEquals(JobState.SUCCEEDED, ended.job.state)
        assertEquals("done", ended.job.result?.steps?.single()?.result)
        assertFalse(ended.reconnecting)
        assertFalse(ended.linkLost)
        val back = await(jobs.connection, "the new agent") { it is ConnectionState.Connected } as ConnectionState.Connected
        assertEquals("1.1.0", back.info.agentVersion)
        // `job.status` is what tells the app that the new version runs, and how the update went.
        assertEquals(listOf("{}", """{"planId":"$planId","after":2}"""), connector.agent.requestsOf("job.status").map { it.toString() })
    }

    @Test
    fun `what the head unit holds is read again once the agent's update has ended on its new version`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        startedJob(stored)
        val old = connector.agent
        old.event("job.progress", 1, progress("restarting"))
        assertTrue(old.phoneLeft())

        // The new version is on trial until `job.status` is asked: before that it lists what the old one had, and
        // after it, what the update brought.
        val state = AtomicReference("off")
        connector.onConnect = { agent ->
            newVersion(agent)
            withSwitch(agent, state)
            agent.handlers["job.status"] = {
                agent.installed = "[$pluginRow]"
                """{"job":${job("succeeded", 2, ""","result":$done""")},"events":[],"more":false}"""
            }
        }

        val back = await(jobs.connection, "what the new agent holds") {
            (it as? ConnectionState.Connected)?.installed?.isNotEmpty() == true
        } as ConnectionState.Connected
        assertEquals(listOf("org.nbtapp.plugin"), back.installed.map { it.id })
        assertEquals(Autostart(on = false, available = true), back.autostart)
        assertEquals(listOf("org.nbtapp.plugin"), headUnits.headUnit("car").first()?.profile?.installed?.map { it.id })
        // Read once before the job's end was known, and once after.
        assertEquals(
            listOf("hello", "info.get", "packages.list", "autostart.get", "job.status", "job.status", "info.get", "packages.list", "autostart.get"),
            methods(connector.agent),
        )
    }

    @Test
    fun `a job that had ended before the link was made leads to no second reading`() = test {
        connector.onConnect = { agent ->
            agent.handlers["job.status"] = { """{"job":${job("succeeded", 3, ""","result":$done""")},"events":[],"more":false}""" }
        }

        assertTrue(jobs.connect("car").isSuccess)

        assertEquals(JobState.SUCCEEDED, jobs.job.value?.job?.state)
        assertEquals(listOf("hello", "info.get", "packages.list", "job.status", "job.status"), methods(connector.agent))
    }

    @Test
    fun `an agent that does not come back is given up on, and the job is then one whose link was lost`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        val jobs = repository(connector, Rejoin(firstAfterMs = 20, pauseMs = 20, giveUpAfterMs = 400))
        startedJob(stored, jobs)
        connector.unreachable = "Nothing listens there"
        val told = System.nanoTime()

        connector.agent.closing(LinkClosing.RESTARTING)

        val failed = await(jobs.connection, "the giving up") { it is ConnectionState.Failed } as ConnectionState.Failed
        assertEquals("The head unit's agent hasn't answered since it restarted. Nothing listens there", failed.reason)
        // The last try begins no later than a pause before the time is up.
        assertTrue("given up on too soon", (System.nanoTime() - told) / 1_000_000 >= 400 - 20)
        val view = requireNotNull(jobs.job.value)
        assertTrue(view.linkLost)
        assertFalse(view.reconnecting)

        // And that was the last try.
        connector.unreachable = null
        delay(300)
        assertEquals(1, connector.agents.size)
        assertEquals(failed, jobs.connection.value)
    }

    @Test
    fun `a disconnect ends the app's own wait, and a connect of the user's takes its place`() = test {
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        // Tries that go on failing for as long as this test lasts.
        val jobs = repository(connector, Rejoin(firstAfterMs = 20, pauseMs = 20, giveUpAfterMs = 600_000))
        startedJob(stored, jobs)
        connector.unreachable = "Nothing listens there yet"
        connector.agent.closing(LinkClosing.RESTARTING)
        await(jobs.job, "the wait") { it?.reconnecting == true }

        jobs.disconnect()

        assertEquals(ConnectionState.Disconnected, jobs.connection.value)
        val view = requireNotNull(jobs.job.value)
        assertTrue(view.linkLost)
        assertFalse(view.reconnecting)
        // Had the wait gone on, its next try would have got through here.
        connector.unreachable = null
        delay(300)
        assertEquals(1, connector.agents.size)
        assertEquals(ConnectionState.Disconnected, jobs.connection.value)

        // The same with a connect in the disconnect's place: one link is opened, the user's.
        assertTrue(jobs.connect("car").isSuccess)
        connector.unreachable = "Nothing listens there yet"
        connector.agent.closing(LinkClosing.RESTARTING)
        await(jobs.connection, "the wait") { it == ConnectionState.Connecting("car") }
        connector.unreachable = null
        assertTrue(jobs.connect("car").isSuccess)
        delay(300)
        assertTrue(jobs.connection.value is ConnectionState.Connected)
    }

    @Test
    fun `where the agent that comes back wants the phone paired first, the app leaves it at that`() = test {
        val phones = guarded()
        keys.put(agentId, phones.pair(phoneId))
        val stored = stored()
        connector.onConnect = { planning(it, stored) }
        startedJob(stored)

        // The agent that comes back holds no key for this phone.
        phones.forgetAll()
        connector.agent.event("job.progress", 1, progress("restarting"))

        assertEquals(
            ConnectionState.NeedsPairing("car", overTcp, stale = true),
            await(jobs.connection, "the pairing") { it is ConnectionState.NeedsPairing },
        )
        // The job cannot be followed until the user has paired the phone: it is one whose link was lost.
        val view = requireNotNull(await(jobs.job, "the lost link") { it?.linkLost == true })
        assertFalse(view.reconnecting)
        // The link that waits for the code is kept, and no other is opened beside it.
        delay(300)
        assertEquals(3, connector.agents.size)
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
    }
}

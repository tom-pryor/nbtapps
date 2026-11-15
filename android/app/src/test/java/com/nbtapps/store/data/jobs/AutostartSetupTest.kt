// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemoryPrereleaseRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.packages.PackageArchive
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.AutostartReason
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageDependency
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageNeed
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.model.VersionRange
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * Setting a head unit up to start the agent with the car: what it takes by what the agent says ([autostartPlan]),
 * which plugin is installed for it ([pluginToInstall]), and the whole run ([AutostartSetup]) over the app's real
 * job repository against a stand-in agent.
 */
class AutostartSetupTest {

    @get:Rule
    val folder = TemporaryFolder()

    // ---- what it takes ----

    private val needs = PackageNeed(PLUGIN, ">=1.0.0")

    @Test
    fun `what it takes follows what the agent says`() {
        assertEquals(AutostartPlan.AlreadyOn, autostartPlan(Autostart(on = true, available = true)))
        assertEquals(AutostartPlan.SwitchOn, autostartPlan(Autostart(on = false, available = true)))
        // A plugin new enough is left as it is, whatever the catalogue has: nothing here looks at it.
        assertEquals(AutostartPlan.SwitchOn, autostartPlan(Autostart(on = false, available = true, starts = 3)))

        val missing = Autostart(on = false, available = false, reason = AutostartReason.PLUGIN_MISSING, needs = needs)
        assertEquals(AutostartPlan.InstallFirst(needs), autostartPlan(missing))
        assertEquals(AutostartPlan.InstallFirst(needs), autostartPlan(missing.copy(reason = AutostartReason.PLUGIN_TOO_OLD)))
        // Switched on with nothing to do the starting: the plugin is wanting all the same.
        assertEquals(AutostartPlan.InstallFirst(needs), autostartPlan(missing.copy(on = true)))
    }

    @Test
    fun `where nothing the app installs would help, it cannot be, and the reason says why`() {
        assertEquals(AutostartPlan.CannotBe(AGENT_WITHOUT_SWITCH), autostartPlan(null))
        assertEquals(
            AutostartPlan.CannotBe(AgentText.autostartReason(AutostartReason.NO_LAUNCHER)),
            autostartPlan(Autostart(on = false, available = false, reason = AutostartReason.NO_LAUNCHER)),
        )
        // A plugin that is wanting with no package named to install, and a reason from a later agent.
        assertEquals(
            AutostartPlan.CannotBe(AgentText.autostartReason(AutostartReason.PLUGIN_MISSING)),
            autostartPlan(Autostart(on = false, available = false, reason = AutostartReason.PLUGIN_MISSING)),
        )
        assertEquals(
            AutostartPlan.CannotBe(AgentText.autostartReason("one from later")),
            autostartPlan(Autostart(on = false, available = false, reason = "one from later", needs = needs)),
        )
    }

    // ---- which plugin ----

    private fun version(version: String, agent: String? = null, vararg depends: Pair<String, String?>) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = version,
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
        depends = depends.map { PackageDependency(it.first, it.second) },
    )

    private fun pkg(id: String, stable: List<PackageVersion>, prereleases: List<PackageVersion> = emptyList(), name: String = id) =
        CataloguePackage(id = id, name = name, versions = stable, prereleases = prereleases)

    private fun indexOf(vararg packages: CataloguePackage) = CatalogueIndex(1, CatalogueSourceInfo("test", "Test"), 1, packages = packages.toList())

    private fun choose(index: CatalogueIndex, installed: List<InstalledPackage> = emptyList(), agent: String = "1.2.0", switchedOn: Set<String> = emptySet()) =
        pluginToInstall(index, needs, installed, agent, switchedOn)

    private fun installs(choice: PluginChoice): String = (choice as PluginChoice.Install).version.version

    @Test
    fun `the plugin installed is the one its page offers on this head unit, inside the range the agent gave`() {
        val plugin = pkg(PLUGIN, listOf(version("1.2.0", ">=2.0.0"), version("1.1.0", ">=1.1.0"), version("0.9.0")))

        // The newest the agent runs, not the newest of all.
        assertEquals("1.1.0", installs(choose(indexOf(plugin))))
        // Over one that is too old, the same.
        assertEquals("1.1.0", installs(choose(indexOf(plugin), installed = listOf(InstalledPackage(PLUGIN, "Plugin", "0.9.0")))))
        // With its pre-release versions switched on, a newer pre-release is offered, as on its page.
        val beta = pkg(PLUGIN, plugin.versions, listOf(version("1.1.1-beta.1", ">=1.1.0")))
        assertEquals("1.1.0", installs(choose(indexOf(beta))))
        assertEquals("1.1.1-beta.1", installs(choose(indexOf(beta), switchedOn = setOf(PLUGIN))))
    }

    @Test
    fun `with no plugin the catalogue could install for it, the set-up says why`() {
        // Not in the catalogue at all.
        assertEquals(
            PluginChoice.Unavailable("The catalogue doesn't have the iDrive plugin."),
            choose(indexOf(pkg("org.nbtapp.hello", listOf(version("1.0.0"))))),
        )
        // Every version needs a newer agent.
        val tooNew = choose(indexOf(pkg(PLUGIN, listOf(version("1.1.0", ">=9.0.0"), version("1.0.0", ">=9.0.0")))))
        assertTrue(tooNew.toString(), (tooNew as PluginChoice.Unavailable).reason.contains("needs a newer agent"))
        // The version on offer is outside the range the agent gave.
        val old = choose(indexOf(pkg(PLUGIN, listOf(version("0.9.0")))))
        assertEquals(PluginChoice.Unavailable("The catalogue has no iDrive plugin that would start this agent."), old)
        // Out only in pre-release versions, with its switch off.
        val prereleaseOnly = indexOf(pkg(PLUGIN, emptyList(), listOf(version("1.0.0-beta.1"))))
        assertTrue((choose(prereleaseOnly) as PluginChoice.Unavailable).reason.contains("pre-release"))
        assertEquals(PluginChoice.Unavailable("The catalogue has no iDrive plugin that would start this agent."), choose(prereleaseOnly, switchedOn = setOf(PLUGIN)))
    }

    @Test
    fun `a plugin that waits for another package's pre-release is not installed`() {
        val plugin = pkg(PLUGIN, listOf(version("1.0.0", null, "runtime" to ">=2.2.0-beta.1")))
        val runtime = pkg("runtime", listOf(version("2.1.0")), listOf(version("2.2.0-beta.1")), name = "NBT Runtime")

        val choice = choose(indexOf(plugin, runtime))

        assertEquals(PluginChoice.Unavailable(waitingFor(listOf(PrereleaseAsk("runtime", "NBT Runtime", "2.2.0-beta.1")))), choice)
        assertEquals("1.0.0", installs(choose(indexOf(plugin, runtime), switchedOn = setOf("runtime"))))
    }

    // ---- the set-up, against a stand-in agent ----

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy { JobRepository(headUnits, connector, identity, MemoryPairingKeys(), store, {}, scope, shareDir = folder.newFolder("share")) }
    private val archives = AgentVectors.file("packages/good")
    private val source = FileCatalogueSource(null, archives)
    private val catalogue = TestCatalogue()
    private val prereleases = InMemoryPrereleaseRepository()
    private val setup by lazy { AutostartSetup(jobs, catalogue, store, PackageDownloader(source, store, scope), prereleases, scope, settleMs = 2_000) }
    private val car = HeadUnit("car", "Car", listOf(TcpEndpoint("192.168.0.1", 7144)))

    /** A catalogue that holds [loaded] once it has been refreshed, and counts its refreshes. */
    private class TestCatalogue : CatalogueRepository {
        @Volatile
        var loaded: CatalogueIndex? = null
        val refreshes = AtomicInteger()
        private val state = MutableStateFlow(CatalogueState())
        override val catalogue: StateFlow<CatalogueState> = state

        override suspend fun refresh() {
            refreshes.incrementAndGet()
            state.value = loaded?.let { CatalogueState(index = it) } ?: CatalogueState(error = "no network")
        }

        override suspend fun details(packageId: String): Result<PackageDetails> = Result.failure(UnsupportedOperationException())
    }

    /** What the stand-in head unit holds, which every agent of the connector says. */
    private class Bench {
        @Volatile
        var on = false

        /** A starter is in the agent's folder. */
        @Volatile
        var launcher = true

        /** The version of the iDrive plugin installed, if any. */
        @Volatile
        var plugin: String? = null

        /** What `autostart.set` is refused with, if anything. */
        @Volatile
        var refuseSet: String? = null

        /** What `job.plan` is refused with, if anything, or the plan problem it answers with. */
        @Volatile
        var refusePlan: String? = null

        @Volatile
        var planProblem: String? = null
    }

    private val bench = Bench()

    /** Each agent's plan has its own id, as the real agent's do. */
    private val plans = AtomicInteger()

    private fun planIdOf(agent: FakeAgent) = "%016x".format(0x2000L + connector.agents.indexOf(agent))

    private fun autostart(): String {
        val plugin = bench.plugin
        return when {
            !bench.launcher -> """{"on":${bench.on},"available":false,"reason":"no-launcher","starts":0}"""
            plugin == null -> """{"on":${bench.on},"available":false,"reason":"plugin-missing","needs":{"id":"$PLUGIN","version":">=1.0.0"},"starts":0}"""
            !VersionRange.matches(plugin, ">=1.0.0") ->
                """{"on":${bench.on},"available":false,"reason":"plugin-too-old","needs":{"id":"$PLUGIN","version":">=1.0.0"},"starts":0}"""
            else -> """{"on":${bench.on},"available":true,"starts":0}"""
        }
    }

    /** Makes [agent] one with the switch that holds what [bench] does, and plans and starts the plugin's job. */
    private fun withSwitch(agent: FakeAgent) {
        agent.capabilities = """["autostart"]"""
        agent.handlers["packages.list"] = {
            val row = bench.plugin?.let { """{"id":"$PLUGIN","name":"NBTApp iDrive plugin","version":"$it","key":"${"ab".repeat(32)}","flags":[]}""" }
            """{"packages":[${row.orEmpty()}],"more":false}"""
        }
        agent.handlers["autostart.get"] = { autostart() }
        agent.handlers["autostart.set"] = { params ->
            bench.refuseSet?.let { throw Refusal(it) }
            val on = params.getValue("on").jsonPrimitive.content == "true"
            if (on && !autostart().contains("\"available\":true")) throw Refusal(if (bench.launcher) "plugin-missing" else "no-launcher")
            bench.on = on
            autostart()
        }
        agent.handlers["job.plan"] = { params ->
            bench.refusePlan?.let { throw Refusal(it) }
            plans.incrementAndGet()
            val version = params.toString().substringAfter("\"version\":\"").substringBefore('"')
            val problems = bench.planProblem?.let { """[{"code":"$it","id":"$PLUGIN"}]""" } ?: "[]"
            val id = if (bench.planProblem == null) """"planId":"${planIdOf(agent)}",""" else ""
            """{$id"steps":[{"action":"install","id":"$PLUGIN","version":"$version"}],"missing":[],"problems":$problems,"flags":["needs-reboot"],""" +
                """"fetchBytes":1,"installBytes":1,"restart":"hmi-restart"}"""
        }
        agent.handlers["job.start"] = { """{"job":{"planId":"${planIdOf(agent)}","state":"fetching","step":1,"steps":1,"id":"$PLUGIN","phase":"fetch","seq":0}}""" }
        agent.handlers["system.restart"] = { "{}" }
    }

    /** Ends the plugin's job on the connector's agent: installed as [version], or failed with [error]. */
    private fun finishJob(version: String, error: String? = null) {
        val agent = connector.agent
        val step = if (error == null) {
            bench.plugin = version
            agent.restartNeeded = "hmi-restart"
            """{"action":"install","id":"$PLUGIN","version":"$version","result":"done"}"""
        } else {
            """{"action":"install","id":"$PLUGIN","version":"$version","result":"failed","error":{"code":"$error","message":"no"}}"""
        }
        val state = if (error == null) "succeeded" else "failed"
        agent.event("job.done", 1, """{"planId":"${planIdOf(agent)}","state":"$state","steps":[$step],"restart":"hmi-restart"}""")
    }

    /** The plugin's catalogue entry for the archive of [version] among the agent repo's test packages. */
    private fun entry(version: String): PackageVersion {
        val file = File(archives, "$PLUGIN-$version.nbtpkg")
        val verified = PackageArchive.verify(file, listOf(AgentVectors.packageKey))
        return PackageVersion(
            version, url = "https://example.invalid/packages/${file.name}", size = verified.size, sha256 = verified.sha256,
            manifestSha256 = verified.manifestSha256,
        )
    }

    private fun catalogueWith(vararg versions: String) {
        catalogue.loaded = indexOf(pkg(PLUGIN, versions.map(::entry), name = "NBTApp iDrive plugin"))
    }

    @After
    fun tearDown() = scope.cancel()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(car)
            store.load()
            connector.onConnect = ::withSwitch
            block()
        }
    }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun ended(): AutostartStep = requireNotNull(await(setup.state, "the set-up to end") { it?.ended == true }).step

    private suspend fun installing(): String =
        requireNotNull((await(setup.state, "the plugin's job") { (it?.step as? AutostartStep.Installing)?.planId != null }?.step as AutostartStep.Installing).planId)

    private fun sets(): List<String> = connector.agents.flatMap { agent -> agent.requestsOf("autostart.set").map { it.toString() } }

    @Test
    fun `an agent that starts with the car already is left as it is`() = test {
        bench.plugin = "1.0.0"
        bench.on = true
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        assertEquals(AutostartStep.Done(changed = false), ended())
        assertEquals("car", setup.state.value?.headUnitId)
        assertEquals(emptyList<String>(), sets())
        assertEquals(0, catalogue.refreshes.get())
    }

    @Test
    fun `a plugin new enough is only switched on, though the catalogue has a newer one`() = test {
        bench.plugin = "1.0.0"
        catalogueWith("1.1.0", "1.0.0")
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        assertEquals(AutostartStep.Done(changed = true), ended())
        assertTrue(bench.on)
        assertEquals(listOf("""{"on":true}"""), sets())
        assertEquals(0, plans.get())
        assertEquals(0, catalogue.refreshes.get())
    }

    @Test
    fun `a missing plugin is downloaded, installed without a plan shown, and then switched on`() = test {
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        val planId = installing()
        // The catalogue was loaded for it, and the archive is on the phone now.
        assertEquals(1, catalogue.refreshes.get())
        assertEquals(listOf("$PLUGIN 1.0.0"), store.packages.value.map { "${it.id} ${it.version}" })
        assertEquals(planId, jobs.job.value?.planId)
        assertEquals(emptyList<String>(), sets())

        finishJob("1.0.0")

        assertEquals(AutostartStep.Done(changed = true), ended())
        assertTrue(bench.on)
        assertEquals(listOf("""{"on":true}"""), sets())
        assertEquals(Autostart(on = true, available = true), (jobs.connection.value as ConnectionState.Connected).autostart)
    }

    @Test
    fun `a plugin too old is updated, from an archive the phone holds already, and iDrive is restarted on asking`() = test {
        bench.plugin = "0.9.0"
        catalogueWith("1.0.0")
        File(archives, "$PLUGIN-1.0.0.nbtpkg").inputStream().use { store.add(it, listOf(AgentVectors.packageKey)) }
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")
        installing()
        finishJob("1.0.0")

        assertEquals(AutostartStep.Done(changed = true), ended())
        assertEquals("1.0.0", bench.plugin)
        assertEquals("hmi-restart", (jobs.connection.value as ConnectionState.Connected).info.restartNeeded)

        setup.restart()

        assertEquals(AutostartStep.Restarting, await(setup.state, "restarting") { it?.step == AutostartStep.Restarting }?.step)
        assertEquals("hmi", connector.agent.requestsOf("system.restart").single().getValue("kind").jsonPrimitive.content)
        // It has ended, and goes once dismissed.
        setup.dismiss()
        assertEquals(null, setup.state.value)
    }

    @Test
    fun `an iDrive restart the agent refuses is shown, and the set-up stays done`() = test {
        bench.plugin = "1.0.0"
        assertTrue(jobs.connect("car").isSuccess)
        connector.agent.handlers["system.restart"] = { throw Refusal("busy") }
        setup.begin("car")
        assertEquals(AutostartStep.Done(changed = true), ended())

        setup.restart()

        val refused = await(setup.state, "the refusal") { (it?.step as? AutostartStep.Done)?.restartError != null }
        assertEquals(AutostartStep.Done(changed = true, restartError = AgentText.request("busy")), refused?.step)
    }

    @Test
    fun `a switch the agent refuses as busy fails in the app's words, and Retry switches it on`() = test {
        bench.plugin = "1.0.0"
        bench.refuseSet = "busy"
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        assertEquals(AutostartStep.Failed(AgentText.autostart("busy")), ended())
        bench.refuseSet = null

        setup.retry()

        assertEquals(AutostartStep.Done(changed = true), ended())
        assertTrue(bench.on)
    }

    @Test
    fun `a plugin that went in before a failure is not installed again on Retry`() = test {
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)
        setup.begin("car")
        installing()
        bench.refuseSet = "io-error"
        finishJob("1.0.0")
        assertEquals(AutostartStep.Failed(AgentText.autostart("io-error")), ended())

        bench.refuseSet = null
        setup.retry()

        assertEquals(AutostartStep.Done(changed = true), ended())
        assertEquals(1, plans.get())
    }

    @Test
    fun `a head unit busy with another job fails the install, and says so`() = test {
        bench.refusePlan = "busy"
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        assertEquals(AutostartStep.Failed(AgentText.request("busy")), ended())
    }

    @Test
    fun `a plan the agent says cannot run fails with its problem in the app's words`() = test {
        bench.planProblem = "stock-mismatch"
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        assertEquals(AutostartStep.Failed(AgentText.problem("stock-mismatch", "NBTApp iDrive plugin")), ended())
        assertEquals(emptyList<String>(), sets())
    }

    @Test
    fun `a job that fails fails the set-up with the step's error`() = test {
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)
        setup.begin("car")
        installing()

        finishJob("1.0.0", error = "no-space")

        assertEquals(AutostartStep.Failed(AgentText.problem("no-space", "NBTApp iDrive plugin")), ended())
        assertEquals(emptyList<String>(), sets())
    }

    @Test
    fun `an agent from before the switch cannot be set up, and nothing is asked of it`() = test {
        connector.onConnect = {}
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        assertEquals(AutostartStep.Failed(AGENT_WITHOUT_SWITCH), ended())
        assertEquals(emptyList<String>(), sets())
        assertEquals(0, catalogue.refreshes.get())
    }

    @Test
    fun `an agent with no starter cannot be set up, and the reason is the agent's`() = test {
        bench.launcher = false
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)

        setup.begin("car")

        assertEquals(AutostartStep.Failed(AgentText.autostartReason(AutostartReason.NO_LAUNCHER)), ended())
        assertEquals(0, plans.get())
    }

    @Test
    fun `a catalogue that cannot be loaded, and a download that fails, end the set-up with a reason`() = test {
        assertTrue(jobs.connect("car").isSuccess)
        setup.begin("car")
        val unloaded = ended() as AutostartStep.Failed
        assertTrue(unloaded.reason, unloaded.reason.startsWith("The catalogue couldn't be loaded."))

        catalogueWith("1.0.0")
        source.unreachable = "no network"
        setup.retry()

        assertEquals(AutostartStep.Failed("The iDrive plugin couldn't be downloaded: no network"), ended())
        assertEquals(0, plans.get())
    }

    @Test
    fun `a link lost during the install fails the set-up, and Retry over a new link goes on`() = test {
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)
        setup.begin("car")
        installing()

        connector.agent.drop()

        val lost = ended() as AutostartStep.Failed
        assertTrue(lost.reason, lost.reason.startsWith("The app lost its link to the head unit."))
        // With no link there is nothing to set up.
        setup.retry()
        val none = ended() as AutostartStep.Failed
        assertTrue(none.reason, none.reason.startsWith("The app has no link to the head unit."))

        assertTrue(jobs.connect("car").isSuccess)
        setup.retry()
        installing()
        finishJob("1.0.0")

        assertEquals(AutostartStep.Done(changed = true), ended())
        assertEquals(2, plans.get())
    }

    @Test
    fun `a set-up under way is left alone by a second begin for the same head unit`() = test {
        catalogueWith("1.0.0")
        assertTrue(jobs.connect("car").isSuccess)
        setup.begin("car")
        val planId = installing()

        setup.begin("car")
        setup.dismiss()

        assertEquals(AutostartStep.Installing(planId), setup.state.value?.step)
        finishJob("1.0.0")
        assertEquals(AutostartStep.Done(changed = true), ended())
        assertEquals(1, plans.get())
    }

    private companion object {
        const val PLUGIN = "org.nbtapp.plugin"
    }
}

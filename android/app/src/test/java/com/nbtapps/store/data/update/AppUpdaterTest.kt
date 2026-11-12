// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.update

import java.io.File
import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class AppUpdaterTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val body = apkBytes("0.2.0")
    private val release = release("0.2.0", 200, body)
    private val source = ApkSource(body)
    private val installer = FakeApkInstaller()
    private var facts: ApkFacts? = ApkFacts(APP_PACKAGE, 200, RELEASE_CERT)
    private var busy = listOf(false)
    private var asked = 0
    private val dir by lazy { File(folder.root, "update") }

    private val updater by lazy {
        AppUpdater(
            source = source,
            installed = RUNNING,
            dir = dir,
            inspector = { facts },
            installer = installer,
            // Each asking takes the next answer, and the last one holds from then on.
            busy = { busy[minOf(asked++, busy.size - 1)] },
            scope = scope,
        )
    }

    @After
    fun tearDown() = scope.cancel()

    private fun <T> StateFlow<T>.await(what: String, predicate: (T) -> Boolean): T = runBlocking {
        withTimeoutOrNull(10_000) { first(predicate) } ?: throw AssertionError("never came: $what; last was $value")
    }

    /** Waits until Android has been handed its [n]th file by this updater, which then says it is installing. */
    private fun handedOver(n: Int = 1) {
        installer.count.await("hand-over $n") { it == n }
        updater.state.await("the install") { it == UpdateState.Installing }
    }

    private fun failure(): String = (updater.state.await("a failure") { it is UpdateState.Failed } as UpdateState.Failed).reason

    /** Android's answer, once the updater listens for it. */
    private fun answer(event: InstallEvent) = runBlocking {
        installer.events.subscriptionCount.first { it > 0 }
        installer.events.emit(event)
    }

    private fun files() = dir.listFiles().orEmpty().map { it.name }.sorted()

    @Test
    fun `a release is downloaded, checked and handed to Android`() {
        updater.start(release)
        handedOver()

        assertEquals(listOf(release.url), source.opened)
        assertArrayEquals(body, installer.handed.single())
        assertEquals(listOf("nbtapps-200.apk"), files())
    }

    @Test
    fun `Android's question and its answers are the update's state`() {
        updater.start(release)
        handedOver()

        answer(InstallEvent.NeedsUser)
        updater.state.await("the question") { it == UpdateState.Confirming }
        answer(InstallEvent.Failed("The update was cancelled."))
        assertEquals("The update was cancelled.", failure())

        // Another try starts from the beginning, and an install that Android reports done leaves nothing under way.
        updater.start(release)
        handedOver(2)
        answer(InstallEvent.Done)
        updater.state.await("the end") { it == UpdateState.Idle }
        assertEquals(2, installer.handed.size)
    }

    @Test
    fun `an answer to no install of this run changes nothing`() {
        assertEquals(UpdateState.Idle, updater.state.value)
        answer(InstallEvent.Failed("left over"))
        answer(InstallEvent.NeedsUser)
        // A third is taken only once the first two have been dealt with, and none of the three is this run's.
        answer(InstallEvent.Done)
        assertEquals(UpdateState.Idle, updater.state.value)
    }

    @Test
    fun `a download that is not the file the catalogue names is not handed on`() {
        source.body = apkBytes("0.2.0").also { it[0] = 'T'.code.toByte() }
        updater.start(release)
        assertEquals("The download isn't the file the catalogue names.", failure())
        assertTrue(installer.handed.isEmpty())
        assertEquals(emptyList<String>(), files())
    }

    @Test
    fun `a download is stopped once it is longer than the catalogue says, and refused when it is shorter`() {
        source.body = body + ByteArray(200_000)
        updater.start(release)
        assertEquals("The download is larger than the catalogue says.", failure())

        source.body = body.copyOf(body.size - 1)
        updater.start(release)
        updater.state.await("the second try") { it is UpdateState.Failed && it.reason.startsWith("The download ended early") }
        assertTrue(installer.handed.isEmpty())
        assertEquals(emptyList<String>(), files())
    }

    @Test
    fun `an address that cannot be reached says so`() {
        source.body = null
        updater.start(release)
        assertEquals("Couldn't reach catalogue.example. Check the phone's internet connection.", failure())
    }

    @Test
    fun `a file with the right hash still has to be this app`() {
        for ((wrong, words) in listOf(
            null to "The download isn't an Android app.",
            ApkFacts("org.other.app", 200, RELEASE_CERT) to "The download is another app (org.other.app).",
            ApkFacts(APP_PACKAGE, 150, RELEASE_CERT) to "The download is version code 150, not the 200 the catalogue names.",
            ApkFacts(APP_PACKAGE, 200, OTHER_CERT) to "The download is signed with another key than this app.",
        )) {
            facts = wrong
            updater.start(release)
            updater.state.await(words) { it == UpdateState.Failed(words) }
            assertEquals(emptyList<String>(), files())
        }
        assertTrue(installer.handed.isEmpty())
    }

    @Test
    fun `no update begins while a job needs the phone`() {
        busy = listOf(true)
        updater.start(release)
        assertEquals("A job is running on a head unit. Update the app once it has finished.", failure())
        assertEquals(emptyList<String>(), source.opened)
    }

    @Test
    fun `a job that began during the download keeps the update from Android`() {
        busy = listOf(false, true)
        updater.start(release)
        assertEquals("A job is running on a head unit. Update the app once it has finished.", failure())
        assertEquals(listOf(release.url), source.opened)
        assertTrue(installer.handed.isEmpty())

        // Once the job is over the update goes through.
        busy = listOf(false)
        updater.start(release)
        handedOver()
        assertArrayEquals(body, installer.handed.single())
    }

    @Test
    fun `an update that is under way is not started again`() {
        source.gate = CompletableDeferred()
        updater.start(release)
        updater.start(release)
        updater.state.await("the download") { it is UpdateState.Downloading }
        updater.start(release)
        requireNotNull(source.gate).complete(Unit)
        handedOver()
        updater.start(release)

        assertEquals(1, source.opened.size)
        assertEquals(1, installer.handed.size)
    }

    @Test
    fun `an install that Android will not take is a failure`() {
        installer.refusal = IOException("Android didn't take the update. (no room)")
        updater.start(release)
        assertEquals("Android didn't take the update. (no room)", failure())
    }

    @Test
    fun `what an earlier run left is removed before anything is downloaded`() {
        dir.mkdirs()
        File(dir, "nbtapps-150.apk").writeText("an update that is done with")
        File(dir, "nbtapps-180.apk.part").writeText("half of one")

        updater.start(release)
        handedOver()
        assertEquals(listOf("nbtapps-200.apk"), files())
    }

    @Test
    fun `nothing is downloaded until Android lets the app install apps, and the update goes on once it does`() {
        installer.allowed = false
        updater.start(release)
        updater.state.await("the wait to be allowed") { it == UpdateState.NeedsPermission }
        assertEquals(emptyList<String>(), source.opened)

        // Back in front with nothing changed: it waits on.
        updater.permissionMayHaveChanged()
        assertEquals(UpdateState.NeedsPermission, updater.state.value)
        assertEquals(emptyList<String>(), source.opened)

        installer.allowed = true
        updater.permissionMayHaveChanged()
        handedOver()
        assertArrayEquals(body, installer.handed.single())
    }

    @Test
    fun `coming back in front starts nothing that was not asked for`() {
        updater.permissionMayHaveChanged()
        assertEquals(UpdateState.Idle, updater.state.value)

        // Nor does it start a failed update again: that is the user's to try.
        source.body = null
        updater.start(release)
        failure()
        source.body = body
        updater.permissionMayHaveChanged()
        assertTrue(updater.state.value is UpdateState.Failed)
        assertEquals(1, source.opened.size)
    }
}

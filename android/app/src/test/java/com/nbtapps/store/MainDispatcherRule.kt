// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.jobs.FakeConnector
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.CoroutineContext
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.Runnable
import kotlinx.coroutines.cancel
import kotlinx.coroutines.job
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.TestDispatcher
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.rules.TestWatcher
import org.junit.runner.Description

/**
 * Sets Dispatchers.Main for a JVM test, so that viewModelScope works, and ends what the test made before it unsets it.
 *
 * [main] is [testDispatcher] unless a test wants another: the set-up's tests use `Dispatchers.Unconfined`, which
 * waits in real time. Either way a view model's coroutine runs on whichever thread wakes it, and over a stand-in link
 * that is the link's reader thread. So a view model, a scope or a link that outlives its test can go on running there
 * after the test, and reach for Dispatchers.Main once it is unset; the error then surfaces in whichever test next uses
 * `runTest`, as `UncaughtExceptionsBeforeTest`, far from the test that caused it.
 *
 * To prevent that, a test hands what it makes to the rule ([track], [letGo], [closeAtEnd]), and at the end the rule
 * lets go of held answers, cancels the view models and scopes, ends the stand-in links (a cancel does not end a read
 * that waits on one), and waits for all of it to end. What is still running 5 s later fails the test. A test that
 * also has a `TemporaryFolder` declares this rule with `order = 1`, inside the folder's, so that nothing still writes
 * to the folder once it is deleted.
 *
 * Between tests Dispatchers.Main is not unset but [After], which names the last test that set it: a coroutine
 * that slipped through anyway says where it came from, and the next test that uses this rule fails, saying so.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class MainDispatcherRule(
    val testDispatcher: TestDispatcher = UnconfinedTestDispatcher(),
    private val main: CoroutineDispatcher = testDispatcher,
) : TestWatcher() {

    private val letGo = CopyOnWriteArrayList<() -> Unit>()
    private val viewModels = CopyOnWriteArrayList<ViewModel>()
    private val scopes = CopyOnWriteArrayList<CoroutineScope>()
    private val connectors = CopyOnWriteArrayList<FakeConnector>()
    private val closes = CopyOnWriteArrayList<() -> Unit>()

    /** A view model whose scope ends with the test. */
    fun <T : ViewModel> track(viewModel: T): T = viewModel.also { viewModels += it }

    /** A scope that ends with the test. */
    fun <T : CoroutineScope> track(scope: T): T = scope.also { scopes += it }

    /** A stand-in connector, each of whose agents' links ends with the test. */
    fun track(connector: FakeConnector): FakeConnector = connector.also { connectors += it }

    /** Runs [block] first at the end, to let go of an answer or a connect the test holds back, should it fail first. */
    fun letGo(block: () -> Unit) {
        letGo += block
    }

    /** Runs [block] at the end, after the cancels: to end a link that is not a [FakeConnector]'s, or stop an agent. */
    fun closeAtEnd(block: () -> Unit) {
        closes += block
    }

    /**
     * Ends what the test made, and waits for it to end: see the class. The rule does this when the test ends; a test
     * whose own `@After` has more to do once all of it has ended (deletes the folder an agent ran in) calls it first.
     */
    fun end() {
        val viewModels = viewModels.toList()
        val scopes = scopes.toList()
        try {
            letGo.forEach { it() }
            viewModels.forEach { it.viewModelScope.cancel() }
            scopes.forEach { it.cancel() }
            connectors.forEach { connector -> connector.agents.forEach { it.drop() } }
            closes.forEach { it() }
            val jobs = viewModels.map { it.viewModelScope.coroutineContext.job } + scopes.map { it.coroutineContext.job }
            val ended = runBlocking { withTimeoutOrNull(5_000) { jobs.forEach { it.join() } } != null }
            if (!ended) {
                val running = jobs.filter(Job::isActive)
                throw AssertionError("still running 5 s after the test ended, and so free to run on into the next test: $running")
            }
        } finally {
            letGo.clear()
            this.viewModels.clear()
            this.scopes.clear()
            connectors.clear()
            closes.clear()
        }
    }

    override fun starting(description: Description) {
        Dispatchers.setMain(main)
        strayed.getAndSet(null)?.let { throw AssertionError("not this test's doing: ${it.message}", it) }
    }

    override fun finished(description: Description) {
        try {
            end()
        } finally {
            Dispatchers.setMain(After(description.displayName))
        }
    }

    /** Dispatchers.Main between tests: any use of it is an error that names [test], the last test that set it. */
    private class After(private val test: String) : CoroutineDispatcher() {
        override fun isDispatchNeeded(context: CoroutineContext) = true

        override fun dispatch(context: CoroutineContext, block: Runnable) {
            val error = IllegalStateException(
                "Dispatchers.Main was used after $test ended, with no test running that set it: a coroutine of that " +
                    "test, or of one with no MainDispatcherRule after it, outlived it ($context)",
            )
            strayed.compareAndSet(null, error)
            throw error
        }
    }

    private companion object {
        /** The first use of Dispatchers.Main between tests since a test last set it, if there was one. */
        val strayed = AtomicReference<IllegalStateException?>()
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.link.AgentProofException
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.Blob
import com.nbtapps.store.data.link.JobEvent
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkClosedException
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.link.ProtocolException
import com.nbtapps.store.data.link.RemoteException
import com.nbtapps.store.data.link.monotonicMillis
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.Hello
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.JobLog
import com.nbtapps.store.model.JobSnapshot
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.LinkClosing
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.PlanRequest
import com.nbtapps.store.model.RemoveTarget
import com.nbtapps.store.model.inConnectOrder
import java.io.IOException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull

/** Whether the app has a link to a head unit's agent, and to which one. */
sealed interface ConnectionState {
    data object Disconnected : ConnectionState

    data class Connecting(val headUnitId: String) : ConnectionState

    data class Connected(
        /** This app's id of the head unit, as in [HeadUnit.id]. */
        val headUnitId: String,
        val endpoint: Endpoint,
        val info: AgentInfo,
        val installed: List<InstalledPackage>,
        /**
         * Whether the agent is started with the head unit, and whether it can be there. Null for an agent that has
         * no such switch: one from before there was one.
         */
        val autostart: Autostart? = null,
    ) : ConnectionState

    /**
     * A link is open and the agent answered `hello` on it, but it will tell this phone nothing more until the
     * phone is paired: the user opens the pairing screen on the car and types the code it shows into the app
     * ([JobRepository.pair]). The link is held open meanwhile, since the code goes over it.
     */
    data class NeedsPairing(
        val headUnitId: String,
        val endpoint: Endpoint,
        /** The phone had a key for this head unit, and the head unit no longer takes it: its phones were forgotten. */
        val stale: Boolean,
    ) : ConnectionState

    /** The last attempt failed, or the link was lost. [reason] is for the user. */
    data class Failed(val headUnitId: String, val reason: String) : ConnectionState
}

/** What [JobRepository.connect] fails with when it ends in [ConnectionState.NeedsPairing]: the link is up, and of no use yet. */
class PairingNeeded : IOException("This phone has to be paired with the head unit first")

/** The agent no longer takes the pairing key this phone holds for it. */
private class StaleKey : IOException("The head unit no longer knows this phone")

/** A job on a head unit as the app last saw it. */
data class JobView(
    val planId: String,
    /** This app's id of the head unit the job runs on. */
    val headUnitId: String,
    /** The plan the job was started from. Null for a job this app did not start, such as one found on connecting. */
    val plan: Plan?,
    val job: JobSnapshot,
    val log: List<JobLog> = emptyList(),
    /** The link ended before the job did. While it was fetching, it now waits on the head unit for a reconnect. */
    val linkLost: Boolean = false,
    /** The head unit no longer knows the job: it lost power or its agent restarted before the install began. */
    val forgotten: Boolean = false,
    /**
     * The agent ended the link to restart, as it does for its own update, and the app is connecting again by
     * itself to see how the job went: see [Rejoin]. It ends in the job as the head unit has it, or in [linkLost].
     */
    val reconnecting: Boolean = false,
) {
    val ended: Boolean get() = job.ended

    /**
     * The job goes on only while this app runs and holds its link, or is getting it back by itself. Not so once
     * it has ended, or its link is lost for good, or the head unit no longer knows it.
     */
    val needsPhone: Boolean get() = !ended && !linkLost && !forgotten
}

/** A request the agent refused. [code] is the protocol's; the message is the app's own text for it. */
class AgentRefusal(val code: String, message: String) : Exception(message)

/**
 * How long the answer to `hello` is waited for, and how often a Bluetooth serial link that got none is opened
 * again: see `JobRepository.greet`. Times in milliseconds; a test shortens them.
 */
class Greeting(
    /** The wait on the last try, which on any link but a serial one is the only try. */
    val timeoutMs: Long = 10_000,
    /** How many serial links are opened at most: the first, and one more after each that got no answer. */
    val serialTries: Int = 3,
    /** The wait on a serial link that another may follow. The agent answers at once on a link it has taken. */
    val retryTimeoutMs: Long = 4_000,
    /** The pause before the next link is opened, for the car to take the last one down. */
    val pauseMs: Long = 700,
)

/**
 * When the app connects again by itself after the agent ended the link to restart: see `JobRepository.rejoin`.
 * Times in milliseconds; a test shortens them.
 */
class Rejoin(
    /**
     * The wait before the first connect. The agent that said it restarts is still there for a moment, taking its
     * links and its service down, and a link to it would only be ended again.
     */
    val firstAfterMs: Long = 2_000,
    /** The pause after a connect that failed. On Bluetooth serial each one has already tried for twenty seconds. */
    val pauseMs: Long = 3_000,
    /** From the end of the link: after this long no further connect is begun, and the user is left to connect. */
    val giveUpAfterMs: Long = 45_000,
)

/** Told when a job starts needing the phone, so the process is kept running for it. */
fun interface JobKeeper {
    fun jobRunning()
}

/**
 * The app's one link to a head unit, and the job that runs over it. The agent is the install engine: this
 * asks it for a plan, starts the plan, answers its fetches through the session's blob server and follows its
 * events. It decides nothing about what gets installed.
 *
 * It lives as long as the process, not as long as a screen: a transfer goes on when the user leaves the
 * screen that started it, and the foreground service keeps the process alive meanwhile.
 *
 * Who may use the link: a phone that is paired with the car's Bluetooth can open one, and so can any app on
 * it, so the agent asks for more. After `hello` this proves with `auth` that the phone holds the pairing key
 * the agent issued it ([keys]); a phone with no key, or with one the agent no longer takes, is paired first
 * ([ConnectionState.NeedsPairing], [pair]). An agent that asks for no `auth`, as one from before there was any
 * and one reached over its own local socket do, is used as it is.
 *
 * How the link ends: the agent says `link.closing` before it ends a link of its own accord, and the session
 * closes the link at once ([AgentSession.closeAs]). What follows depends on why ([LinkClosing]):
 *
 *  - `restarting`: the agent ends for its own update. The app connects again by itself ([rejoin]) and asks
 *    `job.status`, which is how it learns that the new version runs. An agent from before there was the event
 *    says nothing, so a job that reaches `restarting` is taken for the same.
 *  - `forgotten`: the phone's pairing was ended on the car. Its key is dropped, and the next connect pairs anew.
 *  - anything else: the link is shown as failed, with the reason in the app's words ([AgentText.closing]).
 *
 * What the head unit holds is read when the link is made, and again whenever a job has ended, since a job is what
 * changes it. That goes for a job that ended while the app had no link, too: the agent's own update ends on its
 * new version, after the link is back, and only then is what that version brings there to be read ([finish]).
 */
class JobRepository(
    private val headUnits: HeadUnitRepository,
    private val connector: LinkConnector,
    private val identity: PhoneIdentity,
    private val keys: PairingKeys,
    private val store: PackageStore,
    private val keeper: JobKeeper,
    private val scope: CoroutineScope,
    private val now: () -> Long = System::currentTimeMillis,
    private val greeting: Greeting = Greeting(),
    private val rejoin: Rejoin = Rejoin(),
) {
    private val connectionState = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    private val jobState = MutableStateFlow<JobView?>(null)

    val connection: StateFlow<ConnectionState> = connectionState.asStateFlow()

    /** The running job or the last one, on whichever head unit it was. Null until there has been one. */
    val job: StateFlow<JobView?> = jobState.asStateFlow()

    private val connectLock = Mutex()

    /** The link in use: one the agent tells this phone things over. */
    @Volatile
    private var session: AgentSession? = null

    /** A link that is open and greeted, and waits for the phone to be paired: see [ConnectionState.NeedsPairing]. */
    private class Unpaired(val session: AgentSession, val unit: HeadUnit, val endpoint: Endpoint, val hello: Hello)

    @Volatile
    private var unpaired: Unpaired? = null

    /** The blobs of the latest plan, kept so a paused job can be served again over a new link. */
    private class Planned(val planId: String?, val blobs: Map<String, Blob>)

    @Volatile
    private var planned: Planned? = null

    @Volatile
    private var held: Planned? = null

    /** The app's own wait to connect again after the agent said it restarts: see [rejoin]. Null when there is none. */
    @Volatile
    private var rejoining: Job? = null

    /**
     * The head unit, by this app's id, whose agent said on a link that it has forgotten this phone. The next
     * link to it has no key to prove, and is shown as one whose key went stale, which is what happened.
     */
    @Volatile
    private var forgottenBy: String? = null

    /**
     * Opens a link to the head unit [headUnitId] over the first of its endpoints that answers, replacing any
     * link there is. The endpoints are tried in [inConnectOrder]. On success the head unit's profile and
     * installed list are fresh, and a job found running there is followed; a job of this app's that paused
     * when its link dropped goes on.
     *
     * Where the agent wants the phone paired first, the link stays open in [ConnectionState.NeedsPairing] and
     * this fails with [PairingNeeded]; [pair] goes on from there.
     *
     * A caller that is cancelled meanwhile, as a screen's is when the user leaves it, takes the attempt with
     * it: no link stays open, and the state is [ConnectionState.Disconnected].
     */
    suspend fun connect(headUnitId: String): Result<Unit> {
        // What the user asks for takes the place of what the app was waiting to do by itself.
        stopRejoining()
        return connectLock.withLock { attempt(headUnitId, rejoining = false) }
    }

    /**
     * What [connect] does, with [connectLock] held. When [rejoining], it is one of the app's own tries after the
     * agent restarted: a failure is the caller's to publish, and the state stays [ConnectionState.Connecting].
     */
    private suspend fun attempt(headUnitId: String, rejoining: Boolean): Result<Unit> {
        drop("The app connected again")
        val unit = headUnits.headUnit(headUnitId).first()
            ?: return Result.failure(IOException("That head unit is no longer saved"))
        connectionState.value = ConnectionState.Connecting(headUnitId)
        val failures = ArrayList<Pair<Endpoint, String>>()
        var opened: AgentSession? = null
        try {
            for (endpoint in unit.endpoints.filter(connector::supports).inConnectOrder()) {
                opened = null
                val reason = try {
                    val paired = dial(unit, endpoint) { opened = it }
                    return if (paired) Result.success(Unit) else Result.failure(PairingNeeded())
                } catch (e: RemoteException) {
                    AgentText.request(e.code)
                } catch (e: IOException) {
                    e.message ?: "The head unit couldn't be reached"
                }
                opened?.let { abandon(it, reason) }
                failures += endpoint to reason
            }
        } catch (e: CancellationException) {
            // A link that was up but not yet in use must not stay open: on serial it is the car's only one.
            opened?.let { abandon(it, "The connect was given up") }
            connectionState.value = ConnectionState.Disconnected
            throw e
        }
        val reason = when (failures.size) {
            0 -> "This version of the app has no way to reach this head unit yet"
            1 -> failures.single().second
            // Each way that was tried, with why it failed.
            else -> failures.joinToString("\n") { (endpoint, why) -> "${endpoint.label}: $why" }
        }
        if (!rejoining) connectionState.value = ConnectionState.Failed(headUnitId, reason)
        return Result.failure(IOException(reason))
    }

    /**
     * Connects to the head unit [headUnitId] again after its agent ended the link to restart, which it does for
     * its own update: the old version ends, its launcher starts the new one, and that one finishes the job. A
     * connect that succeeds asks `job.status` as every connect does, and so shows how the update went; nothing
     * else tells the phone that the new version runs.
     *
     * The first connect waits [Rejoin.firstAfterMs], for the old agent to be gone. One that fails is tried again
     * until [Rejoin.giveUpAfterMs] have passed since the link ended; then the link is shown as failed, and the
     * user connects. A phone the new agent wants paired first is left at that. [connect] and [disconnect] end
     * the wait.
     */
    private suspend fun rejoin(headUnitId: String) {
        val began = monotonicMillis()
        delay(rejoin.firstAfterMs)
        var failure: String
        while (true) {
            val result = connectLock.withLock {
                // Taking a free lock asks nothing of the caller: one that was told to stop must not connect.
                currentCoroutineContext().ensureActive()
                attempt(headUnitId, rejoining = true)
            }
            if (result.isSuccess) return
            if (result.exceptionOrNull() is PairingNeeded) return lost(headUnitId)
            failure = result.exceptionOrNull()?.message.orEmpty()
            if (monotonicMillis() - began + rejoin.pauseMs >= rejoin.giveUpAfterMs) break
            delay(rejoin.pauseMs)
        }
        connectLock.withLock {
            currentCoroutineContext().ensureActive()
            lost(headUnitId)
            connectionState.value =
                ConnectionState.Failed(headUnitId, "The head unit's agent hasn't answered since it restarted. $failure".trim())
        }
    }

    /** The job the app was connecting again for, if any, is one whose link was lost. */
    private fun lost(headUnitId: String) = jobState.update { view ->
        if (view != null && view.reconnecting && view.headUnitId == headUnitId) view.copy(reconnecting = false, linkLost = true) else view
    }

    /** Ends the app's own wait to connect again, if there is one. */
    private fun stopRejoining() {
        rejoining?.cancel()
        rejoining = null
        jobState.update { view -> if (view != null && view.reconnecting) view.copy(reconnecting = false, linkLost = true) else view }
    }

    /** Closes a session that [connect] or [pair] has and will not use. */
    private fun abandon(opened: AgentSession, reason: String) {
        if (session === opened) session = null
        if (unpaired?.session === opened) unpaired = null
        opened.close(reason)
    }

    /**
     * Pairs this phone with the head unit whose link waits for it in [ConnectionState.NeedsPairing]. [code] is
     * what the car's pairing screen shows. The agent answers a right code with a key, which is kept ([keys]) and
     * proved at once, and the link is then connected as [connect] leaves one.
     *
     * A wrong code, or none on the car's screen, fails with an [AgentRefusal] whose message says which
     * ([AgentText.pairing]) and leaves the link waiting, for another try. A caller that is cancelled meanwhile
     * takes the link with it, as with [connect].
     */
    suspend fun pair(code: String): Result<Unit> = connectLock.withLock {
        val waiting = unpaired ?: return@withLock Result.failure(IOException("No link to a head unit is waiting to be paired"))
        val next = waiting.session
        val agentId = requireNotNull(waiting.hello.headUnitId)
        try {
            try {
                val key = withTimeoutOrNull(greeting.timeoutMs) { next.client.pairingIssue(code) }
                    ?: throw IOException("The head unit didn't answer")
                try {
                    keys.put(agentId, key)
                } finally {
                    key.fill(0)
                }
                prove(next, agentId)
            } catch (e: RemoteException) {
                return@withLock Result.failure(AgentRefusal(e.code, AgentText.pairing(e.code)))
            } catch (e: IOException) {
                return@withLock Result.failure(e)
            }
            // The agent takes this phone now. From here the link is as any that is being connected: what fails, fails it.
            val reason = try {
                val info = next.client.info()
                unpaired = null
                finish(waiting.unit, waiting.endpoint, next, waiting.hello, info)
                return@withLock Result.success(Unit)
            } catch (e: RemoteException) {
                AgentText.request(e.code)
            } catch (e: IOException) {
                e.message ?: "The link to the head unit was lost"
            }
            abandon(next, reason)
            connectionState.value = ConnectionState.Failed(waiting.unit.id, reason)
            Result.failure(IOException(reason))
        } catch (e: CancellationException) {
            abandon(next, "The pairing was given up")
            connectionState.value = ConnectionState.Disconnected
            throw e
        }
    }

    /** Ends the link. A job that is fetching pauses on the head unit; one that is installing finishes there. */
    suspend fun disconnect() {
        stopRejoining()
        connectLock.withLock {
            drop("Disconnected")
            connectionState.value = ConnectionState.Disconnected
        }
    }

    /** Reads the head unit's profile and installed list again, and whether its agent is started with it. */
    suspend fun refresh(): Result<Unit> = withSession { current ->
        val connected = connectionState.value as? ConnectionState.Connected ?: return@withSession
        val info = current.client.info()
        // While a step installs, the list is refused; the one from before stands until the job is done.
        val installed = try {
            current.client.packages()
        } catch (e: RemoteException) {
            if (e.code != "busy") throw e
            connected.installed
        }
        val autostart = autostartOf(current)
        if (session === current) show(connected.headUnitId, connected.endpoint, info, installed, autostart)
    }

    /**
     * Switches on or off whether the agent is started with the head unit, on an agent that has the switch
     * ([ConnectionState.Connected.autostart]). What the agent answers is how things then stand, and is published.
     *
     * Where the agent refuses, the failure is an [AgentRefusal] in the app's words ([AgentText.autostart]), and
     * how things stand is read again: a refusal means that they are not as the app had them.
     */
    suspend fun setAutostart(on: Boolean): Result<Unit> {
        val current = session ?: return Result.failure(IOException(NOT_CONNECTED))
        return try {
            try {
                publish(current, current.client.setAutostart(on))
                Result.success(Unit)
            } catch (e: RemoteException) {
                publish(current, autostartOf(current))
                Result.failure(AgentRefusal(e.code, AgentText.autostart(e.code)))
            }
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    /** How the agent of [current] says it is started with the head unit. Null for one that has no such switch. */
    private suspend fun autostartOf(current: AgentSession): Autostart? {
        if (ProtocolConstants.CAPABILITY_AUTOSTART !in current.client.capabilities) return null
        return try {
            current.client.autostart()
        } catch (e: RemoteException) {
            // It listed the capability and will not say: shown as an agent without the switch, and the link stands.
            null
        }
    }

    private fun publish(current: AgentSession, autostart: Autostart?) = connectionState.update { state ->
        if (session === current && state is ConnectionState.Connected) state.copy(autostart = autostart) else state
    }

    /**
     * Asks the agent what installing [install] and removing [remove] would take, offering the archives
     * [offers]. Nothing on the head unit changes. The agent fetches the manifests it needs over the link
     * before it answers.
     *
     * An archive with the catalogue's detached signature is offered only to an agent that lists
     * [ProtocolConstants.CAPABILITY_DETACHED_SIGNATURE]: an older one would plan it and then check the author's
     * `manifest.sig` at install.
     */
    suspend fun plan(install: List<InstallTarget>, remove: List<RemoveTarget>, offers: List<StoredPackage>): Result<Plan> =
        withSession { current ->
            if (offers.any { it.detachedSignature } &&
                ProtocolConstants.CAPABILITY_DETACHED_SIGNATURE !in current.client.capabilities
            ) {
                throw IOException(NO_DETACHED_SIGNATURE)
            }
            val blobs = HashMap<String, Blob>()
            for (offer in offers) blobs += store.blobs(offer)
            current.blobs.offer(blobs)
            val plan = current.client.plan(PlanRequest(install, remove, offers.map(StoredPackage::offer)))
            planned = Planned(plan.planId, blobs)
            plan
        }

    /** Runs [plan], which must be the one [plan] returned last. From here the agent drives and the phone answers its fetches. */
    suspend fun start(plan: Plan): Result<Unit> = withSession { current ->
        val planId = plan.planId ?: throw IOException("This plan can't run")
        val connected = connectionState.value as? ConnectionState.Connected ?: throw IOException(NOT_CONNECTED)
        current.blobs.jobStarted()
        held = planned?.takeIf { it.planId == planId }
        val snapshot = current.client.start(planId)
        jobState.update { known ->
            // Events of the job may have arrived ahead of the reply.
            if (known != null && known.planId == planId && known.headUnitId == connected.headUnitId) {
                known.copy(plan = plan, job = if (known.job.seq > snapshot.seq) known.job else snapshot)
            } else {
                JobView(planId, connected.headUnitId, plan, snapshot)
            }
        }
        keeper.jobRunning()
    }

    /**
     * Stops the job while it is still fetching: the open stream ends with `blob.error`, then `job.cancel`
     * follows. Once the install has begun the agent refuses, and the job runs to its end.
     */
    suspend fun cancel(): Result<Unit> = withSession { current ->
        val view = jobState.value?.takeIf { !it.ended } ?: throw IOException("No job is running")
        current.blobs.cancel(current.peer)
        current.client.cancel(view.planId)
    }

    /** Restarts iDrive ([kind] `hmi`) or the whole head unit (`reboot`). */
    suspend fun restart(kind: String): Result<Unit> = withSession { current -> current.client.restart(kind) }

    /**
     * Opens a link to [endpoint] and says `hello` on it. [holding] is told of each session while it is open and
     * not yet the caller's, and of null once it has been closed again.
     *
     * A Bluetooth serial link that comes up is not yet a link to the agent: the car gives it to whichever program
     * there reads it first, and the agent lets go a link it took too late to see how it opened. Such a link ends
     * before `hello` is answered, or stays silent. It is closed and another is opened, [Greeting.serialTries]
     * times in all. An answer of any kind, a refusal too, is the agent's and is not tried again.
     */
    private suspend fun greet(endpoint: Endpoint, holding: (AgentSession?) -> Unit): Pair<AgentSession, Hello> {
        var attempt = 0
        while (true) {
            attempt++
            val next = AgentSession(connector.connect(endpoint), scope).also(holding)
            val last = next.kind != Link.SERIAL || attempt >= greeting.serialTries
            val failure = try {
                val hello = withTimeoutOrNull(if (last) greeting.timeoutMs else greeting.retryTimeoutMs) {
                    next.client.hello(identity.id(), identity.name)
                }
                if (hello != null) return next to hello
                "The head unit didn't answer"
            } catch (e: LinkClosedException) {
                e.message ?: "The link to the head unit was lost"
            }
            // The words are the last link's; the ones before it failed in one of the same two ways.
            if (last) throw IOException(if (attempt == 1) failure else "$failure ($attempt links were tried)")
            abandon(next, failure)
            holding(null)
            delay(greeting.pauseMs)
        }
    }

    /**
     * Opens a link to [unit] over [endpoint] and takes it as far as it goes: connected, which is true, or
     * waiting for the phone to be paired, which is false. [holding] is as for [greet].
     *
     * A key the agent no longer takes is of no use again: the agent forgot its phones, or holds another key for
     * this one. It is dropped, and since the agent ends a link on which `auth` failed, a new link is opened, on
     * which the phone has no key and is paired anew.
     */
    private suspend fun dial(unit: HeadUnit, endpoint: Endpoint, holding: (AgentSession?) -> Unit): Boolean {
        var stale = false
        while (true) {
            val (next, hello) = greet(endpoint, holding)
            try {
                return open(unit, endpoint, next, hello, stale)
            } catch (e: StaleKey) {
                // Once: the second link is opened with no key to be stale.
                if (stale) throw e
                stale = true
                keys.remove(requireNotNull(hello.headUnitId))
                abandon(next, e.message.orEmpty())
                holding(null)
                if (next.kind == Link.SERIAL) delay(greeting.pauseMs)
            }
        }
    }

    /** What [dial] does with a link that has been greeted. */
    private suspend fun open(unit: HeadUnit, endpoint: Endpoint, next: AgentSession, hello: Hello, stale: Boolean): Boolean {
        if (unit.headUnitId != null && unit.headUnitId != hello.headUnitId) {
            throw ProtocolException("This isn't the head unit saved as ${unit.name}. Add it as a new one.")
        }
        val agentId = requireNotNull(hello.headUnitId)
        if (keys.has(agentId)) prove(next, agentId)
        // The first thing asked also tells whether the agent takes this phone as it is.
        val info = try {
            next.client.info()
        } catch (e: RemoteException) {
            if (e.code != "unauthenticated") throw e
            val waiting = Unpaired(next, unit, endpoint, hello)
            unpaired = waiting
            connectionState.value = ConnectionState.NeedsPairing(unit.id, endpoint, stale || forgottenBy == unit.id)
            scope.launch { attend(waiting) }
            return false
        }
        finish(unit, endpoint, next, hello, info)
        return true
    }

    /**
     * Does `auth` on [next] with the key kept for the head unit [agentId].
     *
     * @throws StaleKey if the agent does not take the key, or proves to hold another.
     */
    private suspend fun prove(next: AgentSession, agentId: String) {
        try {
            next.client.auth { message -> keys.mac(agentId, message) ?: throw IOException("This phone's pairing key is gone from its key store") }
        } catch (e: RemoteException) {
            when (e.code) {
                "auth-failed" -> throw StaleKey()
                // An agent from before there was `auth`, or a link it trusts as it is. What it answers next shows.
                "unsupported", "forbidden" -> Unit
                else -> throw e
            }
        } catch (e: AgentProofException) {
            throw StaleKey()
        }
    }

    /** Publishes the end of a link that waited to be paired, if it ends still waiting. */
    private suspend fun attend(waiting: Unpaired) {
        val reason = waiting.session.ended.await()
        connectLock.withLock {
            if (unpaired !== waiting) return@withLock
            unpaired = null
            connectionState.value = ConnectionState.Failed(waiting.unit.id, reason)
        }
    }

    /** Makes [next], which the agent answers on, the app's link: reads what the head unit holds and follows its job. */
    private suspend fun finish(unit: HeadUnit, endpoint: Endpoint, next: AgentSession, hello: Hello, info: AgentInfo) {
        // The id is kept only now: before `auth` it was no more than what the other side said of itself.
        headUnits.update(unit.id) { it.copy(headUnitId = hello.headUnitId) }
        val installed = try {
            next.client.packages()
        } catch (e: RemoteException) {
            if (e.code != "busy") throw e
            unit.profile?.installed.orEmpty()
        }
        session = next
        if (forgottenBy == unit.id) forgottenBy = null
        show(unit.id, endpoint, info, installed, autostartOf(next))
        val before = jobState.value
        follow(next, unit.id)
        // A job of this app's that ended while there was no link, as the agent's own update does: it ends on the
        // new version, which the `job.status` just asked is what confirms. What was read above is from before
        // that, so it is read again, as it is after any job that ends with the link up.
        val after = jobState.value
        if (before != null && !before.ended && after != null && after.ended && after.planId == before.planId && after.headUnitId == unit.id) {
            refresh()
        }
        scope.launch { pump(next, unit.id, requireNotNull(hello.headUnitId)) }
    }

    /** Publishes what the agent reported, and keeps it with the head unit for when the phone is not in the car. */
    private suspend fun show(headUnitId: String, endpoint: Endpoint, info: AgentInfo, installed: List<InstalledPackage>, autostart: Autostart?) {
        connectionState.value = ConnectionState.Connected(headUnitId, endpoint, info, installed, autostart)
        val profile = HeadUnitProfile(info.agentVersion, info.manifestVersion, info.free, info.restartNeeded, installed, now())
        headUnits.update(headUnitId) { it.copy(profile = profile) }
    }

    /** Picks up the job the agent has on record, if any, with the log lines this app has not seen. */
    private suspend fun follow(current: AgentSession, headUnitId: String) {
        val known = jobState.value?.takeIf { it.headUnitId == headUnitId }
        val found = current.client.status().job
        if (found == null) {
            // A job of ours that had not ended is gone with the head unit's staging area.
            if (known != null && !known.ended) jobState.value = known.copy(forgotten = true, linkLost = false, reconnecting = false)
            return
        }
        val same = known?.takeIf { it.planId == found.planId }
        var view = (same ?: JobView(found.planId, headUnitId, plan = null, job = found))
            .copy(job = found, linkLost = false, forgotten = false, reconnecting = false)
        var after = same?.job?.seq ?: 0
        val replayed = ArrayList<JobLog>()
        while (true) {
            val status = current.client.status(found.planId, after)
            replayed += status.events.filterIsInstance<JobEvent.Log>().map { it.log }
            status.job?.let { view = view.copy(job = it) }
            after = status.events.lastOrNull()?.seq ?: break
            if (!status.more) break
        }
        view = view.copy(log = (view.log + replayed).takeLast(MAX_LOG_LINES))
        jobState.value = view

        // A job of ours that paused when its link dropped goes on over this one.
        val blobs = held?.takeIf { it.planId == found.planId }
        if (found.state == JobState.PAUSED && blobs != null) {
            current.blobs.offer(blobs.blobs)
            current.blobs.jobStarted()
            try {
                val resumed = current.client.start(found.planId)
                jobState.update { it?.takeIf { view -> view.planId == found.planId }?.copy(job = resumed) ?: it }
                keeper.jobRunning()
            } catch (e: RemoteException) {
                // The agent will not take it up again. The job stays as it is shown, and the user plans anew.
                current.blobs.jobEnded()
            }
        }
    }

    /**
     * Applies the agent's events until the link ends, and then publishes its end: see the class for what an end
     * that the agent announced leads to. [agentId] is the head unit's own id, which its pairing key is kept under.
     */
    private suspend fun pump(current: AgentSession, headUnitId: String, agentId: String) {
        for (event in current.events) {
            apply(headUnitId, event)
            // The agent is about to end for its own update. One that knows `link.closing` says so next; one from
            // before says nothing and takes its end of the link away, and a serial link held after that is what a
            // car's Bluetooth has not got over. So the link is ended here, for both.
            if (event is JobEvent.Progress && event.job.state == JobState.RESTARTING) current.closeAs(LinkClosing.RESTARTING)
            if (event is JobEvent.Done) {
                current.blobs.jobEnded()
                held = null
                refresh()
            }
        }
        val reason = current.ended.await()
        val told = current.closing
        connectLock.withLock {
            if (session !== current) return@withLock
            session = null
            val unfinished = { view: JobView? -> view != null && !view.ended && view.headUnitId == headUnitId }
            if (told == LinkClosing.RESTARTING) {
                // The job is not lost with the link: the app goes after it. The service that keeps the process
                // alive for a job goes on doing so meanwhile.
                jobState.update { if (unfinished(it)) it?.copy(reconnecting = true, linkLost = false) else it }
                connectionState.value = ConnectionState.Connecting(headUnitId)
                rejoining = scope.launch { rejoin(headUnitId) }
                return@withLock
            }
            if (told == LinkClosing.FORGOTTEN) {
                // The key is good for nothing now. Without it the next link goes straight to being paired, where
                // with it one more link would be opened only to have `auth` fail on it.
                forgottenBy = headUnitId
                try {
                    keys.remove(agentId)
                } catch (e: IOException) {
                    // Then the next connect finds it stale, and drops it there.
                }
            }
            // The job is marked first, so whoever sees the failed connection also sees what it did to the job.
            jobState.update { if (unfinished(it)) it?.copy(linkLost = true) else it }
            connectionState.value = ConnectionState.Failed(headUnitId, reason)
        }
    }

    private fun apply(headUnitId: String, event: JobEvent) = jobState.update { known ->
        val same = known?.takeIf { it.planId == event.planId && it.headUnitId == headUnitId }
        // Events come to every controller, so a job another one started shows up here by its first event.
        val view = same ?: JobView(event.planId, headUnitId, plan = null, job = JobSnapshot(event.planId, JobState.INSTALLING))
        // An event already seen: replayed by job.status, or sent while that was being answered.
        if (same != null && event.seq <= same.job.seq) return@update known
        when (event) {
            is JobEvent.Progress -> view.copy(job = event.job.copy(seq = event.seq), linkLost = false, forgotten = false, reconnecting = false)
            is JobEvent.Log -> view.copy(job = view.job.copy(seq = event.seq), log = (view.log + event.log).takeLast(MAX_LOG_LINES))
            is JobEvent.Done -> view.copy(job = view.job.copy(state = event.result.state, seq = event.seq, result = event.result))
        }
    }

    /** Closes the link there is, if any, without publishing a state for it. Call with [connectLock] held. */
    private fun drop(reason: String) {
        unpaired?.let {
            unpaired = null
            it.session.close(reason)
        }
        val current = session ?: return
        session = null
        current.close(reason)
        jobState.update { it?.takeIf { view -> !view.ended }?.copy(linkLost = true) ?: it }
    }

    private suspend fun <T> withSession(block: suspend (AgentSession) -> T): Result<T> {
        val current = session ?: return Result.failure(IOException(NOT_CONNECTED))
        return try {
            Result.success(block(current))
        } catch (e: RemoteException) {
            Result.failure(AgentRefusal(e.code, AgentText.request(e.code)))
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    private companion object {
        const val MAX_LOG_LINES = 2_000
        const val NOT_CONNECTED = "Not connected to a head unit"
        const val NO_DETACHED_SIGNATURE = "This head unit's agent can't install community packages. Update the agent first."
    }
}

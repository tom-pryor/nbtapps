// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.data.AlwaysInForeground
import com.nbtapps.store.data.AppForeground
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.link.AgentClient
import com.nbtapps.store.data.link.AgentProofException
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.Blob
import com.nbtapps.store.data.link.IncomingLink
import com.nbtapps.store.data.link.JobEvent
import com.nbtapps.store.data.link.KnockFallback
import com.nbtapps.store.data.link.KnockUnansweredException
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkClosedException
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.LinkLog
import com.nbtapps.store.data.link.NoCarLinkException
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.link.ProtocolException
import com.nbtapps.store.data.link.RemoteException
import com.nbtapps.store.data.link.monotonicMillis
import com.nbtapps.store.data.logs.LogPull
import com.nbtapps.store.data.logs.LogPullAbout
import com.nbtapps.store.data.logs.LogPullRequest
import com.nbtapps.store.data.logs.LogPullState
import com.nbtapps.store.data.packages.ManifestParser
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.data.settings.SettingsChanges
import com.nbtapps.store.data.settings.SettingsException
import com.nbtapps.store.data.settings.SettingsFailure
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.ChannelSwitch
import com.nbtapps.store.model.Check
import com.nbtapps.store.model.CheckFixResult
import com.nbtapps.store.model.CheckProblem
import com.nbtapps.store.model.CheckState
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.FileListing
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitChecks
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
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.SerialResetState
import com.nbtapps.store.model.SettingEntry
import com.nbtapps.store.model.inConnectOrder
import java.io.File
import java.io.IOException
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.getAndUpdate
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull

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
        /** What the agent listed in its `hello` as its capabilities: the names in [ProtocolConstants]. */
        val capabilities: List<String> = emptyList(),
    ) : ConnectionState {
        /** The agent can send log files: [JobRepository.listLogs] and [JobRepository.startPull]. */
        val files: Boolean get() = ProtocolConstants.CAPABILITY_FILES in capabilities

        /** The agent has the compatibility checks: [JobRepository.ensureChecked] and the calls after it. */
        val checks: Boolean get() = ProtocolConstants.CAPABILITY_CHECKS in capabilities

        /** The agent can have the car end this phone's Bluetooth serial link: [JobRepository.resetSerial]. */
        val serialReset: Boolean get() = ProtocolConstants.CAPABILITY_SERIAL_RESET in capabilities

        /** The agent gives and changes packages' settings: [JobRepository.loadSettings] and [JobRepository.saveSettings]. */
        val settings: Boolean get() = ProtocolConstants.CAPABILITY_SETTINGS in capabilities

        /** The agent has channels to installed packages' programs: [com.nbtapps.store.data.link.AgentSession.openChannel]. */
        val channels: Boolean get() = ProtocolConstants.CAPABILITY_CHANNELS in capabilities
    }

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
        /**
         * What the agent listed in its `hello` as its capabilities, as in [Connected.capabilities]: what it can do
         * is known before the phone is paired, though it does none of it until then.
         */
        val capabilities: List<String> = emptyList(),
    ) : ConnectionState

    /**
     * The last attempt failed, or the link was lost. [reason] is for the user. [listenedOnly]: the connect only waited
     * for the car's link, or knocked for it, and none came ([NoCarLinkException]), so the user may have the phone
     * connect itself ([JobRepository.connectFromPhone]).
     */
    data class Failed(val headUnitId: String, val reason: String, val listenedOnly: Boolean = false) : ConnectionState
}

/**
 * For a wait on [won] that was cancelled with [cause]: where nothing had won yet, [won] ends with [cause]. Where
 * something had, which the waiter will now never take, the link [linkOf] finds in it, if any, goes to [keep].
 */
@OptIn(ExperimentalCoroutinesApi::class)
internal fun <T> keepWhatWon(won: CompletableDeferred<T>, cause: CancellationException, linkOf: (T) -> Link?, keep: (Link) -> Unit) {
    if (won.completeExceptionally(cause) || won.getCompletionExceptionOrNull() != null) return
    linkOf(won.getCompleted())?.let(keep)
}

/** What [JobRepository.connect] fails with when it ends in [ConnectionState.NeedsPairing]: the link is up, and of no use yet. */
class PairingNeeded : IOException("This phone has to be paired with the head unit first")

/** The agent no longer takes the pairing key this phone holds for it. */
private class StaleKey : IOException("The head unit no longer knows this phone")

/** A request a connect made, [method], that had no answer in [Greeting.answerMs]. */
private class Unanswered(val method: String) : IOException(NO_ANSWER)

/** Why a connect failed whose agent did not answer. */
private const val NO_ANSWER = "The head unit didn't answer"

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
    /**
     * With [linkLost]: the app's own wait to connect again after the agent restarted ran out of time with no link
     * ([JobRepository.rejoin]), rather than being ended by the user or by a link that failed.
     */
    val waitedOut: Boolean = false,
) {
    val ended: Boolean get() = job.ended

    /**
     * The job goes on only while this app runs and holds its link, or is getting it back by itself. Not so once
     * it has ended, or its link is lost for good, or the head unit no longer knows it.
     */
    val needsPhone: Boolean get() = !ended && !linkLost && !forgotten
}

/**
 * A request the agent refused. [code] is the protocol's; the message is the app's own text for it. [error] is the
 * agent's error object as it came, for a refusal that says more than its code, as `invalid` does with its `keys`.
 */
class AgentRefusal(val code: String, message: String, val error: JsonObject = JsonObject(emptyMap())) : Exception(message)

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
    /**
     * The pause before the next link is opened, for the car to take the last one down. A link the car opens to the
     * phone meanwhile, for an agent that dials, ends it and is the next link: see `JobRepository.greet`.
     */
    val pauseMs: Long = 700,
    /**
     * The wait for the answer to each request a connect makes after `hello`: `auth`, `info.get`, `packages.list`,
     * `autostart.get` and `job.status`. One with none ends the link, and on Bluetooth serial another is opened,
     * within [serialTries]: see `JobRepository.dial`. The requests made once the link is up wait as long as it takes.
     */
    val answerMs: Long = 10_000,
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
    /**
     * To a head unit whose agent dials only when knocked ([HeadUnit.serialKnock]), each connect is one burst of knocks,
     * and one begins this long after the one before began, or as soon as that one is over where it took longer.
     */
    val knockEveryMs: Long = 3_000,
    /** As [giveUpAfterMs], to such a head unit: its agent comes back only to a knock, which the app keeps making until then. */
    val knockGiveUpAfterMs: Long = 90_000,
)

/** How asking the car to end this phone's Bluetooth serial link went: see [JobRepository.resetSerial]. */
sealed interface SerialResetOutcome {
    /**
     * The car was asked to end the link the app had, and [endedByCar] says whether it did so in the time allowed;
     * where it did not, the app ended the link itself. Then the app connected again by itself, and [connected] says
     * whether that link is up.
     */
    data class Sent(val endedByCar: Boolean, val connected: Boolean) : SerialResetOutcome

    /** No Bluetooth serial link was up on the head unit: the car ends the next one that opens within [forMs]. */
    data class Armed(val forMs: Long) : SerialResetOutcome
}

/**
 * Told when something starts that the process is kept running for, under the foreground service: a job that needs
 * the phone, a pull of logs, a terminal being opened.
 */
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
 *  - `other-link`: of a Bluetooth serial link the phone opened and one the car dialled, the agent kept the other.
 *    The app uses one link at a time and closes any other before its `hello`, so where the one that stays is the
 *    app's, the app has moved on to it already and the end of this one shows nothing ([pump]). Otherwise the app
 *    holds no link the agent kept, and this one is shown as failed, as below.
 *  - anything else: the link is shown as failed, with the reason in the app's words ([AgentText.closing]).
 *
 * What the head unit holds is read when the link is made, and again whenever a job has ended, since a job is what
 * changes it. That goes for a job that ended while the app had no link, too: the agent's own update ends on its
 * new version, after the link is back, and only then is what that version brings there to be read ([finish]).
 *
 * No link is held idle while the app is out of the user's sight ([foreground]): Android may freeze a process in the
 * background with its socket open, and a phone that answers the agent long after the agent gave the link up spoils
 * the car's next links. So when the app leaves the foreground with nothing using the link, the link is ended from
 * this side and the head unit remembered; when it comes back, the app connects to that head unit again by itself.
 * A job that needs the phone, a pull of logs, a call that waits for the agent's answer ([usingLink]), and an open
 * terminal ([usingSession]) keep the link for as long as they last: see [watchForeground].
 *
 * An agent that dials has the car open a link to the phone by itself ([ProtocolConstants.CAPABILITY_SERIAL_DIAL]).
 * The app takes such a link whenever it would want one and has none: while it is in front of the user or something
 * keeps it busy, with Bluetooth on, no link and no connect under way, and the user has not disconnected since they
 * last connected or the app last came to the foreground. A link the car opens then is taken as a connect of the app's
 * own is, quietly: see [watchIncoming]. A connect under way listens for the car by itself, through the same listener
 * ([LinkConnector.connect]).
 *
 * Where such an agent says in `info.get` that a link this phone opens itself is left to another program on the car
 * ([HeadUnit.phoneOpenedLeft]), a connect to that head unit only listens, and fails, with nothing of the phone's
 * tried, where the car's link does not come ([ConnectionState.Failed.listenedOnly]); the user may then have the phone
 * connect itself, once ([connectFromPhone]). A link the phone does open while the car may dial gives way to the car's
 * link where that comes before the agent answers it ([greet]).
 *
 * An agent that lists [ProtocolConstants.CAPABILITY_SERIAL_KNOCK] ([HeadUnit.serialKnock], kept from every `hello`)
 * has the car dial the phone only when the app knocks. Every connect to such a head unit knocks ([LinkConnector.knock])
 * in place of all of that: the user's, the app's own on coming back and after a reset, and its wait after the agent
 * restarted ([rejoin]). The phone's own link is then made only where the user asks for it ([connectFromPhone]), which
 * is offered where a knock brought no link, with what the phone can tell of why ([knockFailure]).
 *
 * What is saved of a head unit is learnt from its `hello`, so one whose agent another phone has since updated to one
 * that dials only when knocked is connected to the old way until a `hello` says otherwise. Where that connect only
 * listened and had no link, the phone's own connect is offered as for any such failure, and the `hello` on the link it
 * makes saves that the agent takes knocks: from then on every connect knocks.
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
    /**
     * A clock in milliseconds that only goes forward, and goes on while the phone sleeps and while the process is
     * frozen: what a link's keep-alive and its silence are measured on, and the app's own waits to connect again.
     * `SystemClock.elapsedRealtime` in the app.
     */
    private val clock: () -> Long = ::monotonicMillis,
    private val greeting: Greeting = Greeting(),
    private val rejoin: Rejoin = Rejoin(),
    /**
     * Where a pull of logs writes its ZIP, for the share sheet to hand on: the app's cache, which Android may
     * empty, since a pull is done again in a minute. Each pull empties it first.
     */
    private val shareDir: File = File(System.getProperty("java.io.tmpdir"), "nbt-share"),
    /** Whether the user has the app in front of them. A test that has no activities leaves it always so. */
    private val foreground: AppForeground = AlwaysInForeground,
    /** How long [resetSerial] waits for the car to end the link, in milliseconds; a test shortens it. */
    private val resetWaitMs: Long = 5_000,
    /** Whether the phone's Bluetooth is on and the app may use it: [com.nbtapps.store.data.PhoneBluetooth.on]. */
    private val bluetoothOn: StateFlow<Boolean> = MutableStateFlow(true),
    /**
     * The addresses of the cars that hold the phone over hands-free or audio now, in upper case:
     * [com.nbtapps.store.data.PhoneBluetooth.connected]. What the app's own tries make of a car's link that did not
     * come goes by it ([quietFailure]), and what the user is told of a knock that brought no link ([knockFailure]).
     */
    private val carsHolding: StateFlow<Set<String>> = MutableStateFlow(emptySet()),
    /**
     * Whether the car at a Bluetooth address has a media audio connection to the phone now, or null where the phone
     * cannot tell: [com.nbtapps.store.data.PhoneBluetooth.mediaAudio]. What the user is told of a knock that brought no
     * link goes by it ([knockFailure]).
     */
    private val mediaAudio: (address: String) -> Boolean? = { null },
    /** Where a link's end, and what came of a link the car opened, are said. */
    private val log: LinkLog = LinkLog.NONE,
) {
    private val connectionState = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    private val jobState = MutableStateFlow<JobView?>(null)

    val connection: StateFlow<ConnectionState> = connectionState.asStateFlow()

    /** The running job or the last one, on whichever head unit it was. Null until there has been one. */
    val job: StateFlow<JobView?> = jobState.asStateFlow()

    private val logPullState = MutableStateFlow<LogPullState?>(null)

    /** The pull of logs that runs, or the last one. Null until there has been one. */
    val logPull: StateFlow<LogPullState?> = logPullState.asStateFlow()

    /** The pull that runs, if one does: see [startPull]. */
    @Volatile
    private var pulling: Job? = null

    private val connectLock = Mutex()

    /** Held while the checks are run or a fix is: one at a time, so they run by themselves once. */
    private val checksLock = Mutex()

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

    /**
     * The app's own wait to connect again after the agent said it restarts: see [rejoin]. Null when there is none.
     * Taken and emptied in one step ([stopRejoining]), since a caller without [connectLock] may do so while a wait
     * is begun under it: one that read it empty and then emptied it would lose a wait set in between.
     */
    private val rejoining = AtomicReference<Job?>()

    /**
     * The head unit, by this app's id, whose agent said on a link that it has forgotten this phone. The next
     * link to it has no key to prove, and is shown as one whose key went stale, which is what happened.
     */
    @Volatile
    private var forgottenBy: String? = null

    /**
     * The calls that wait for the agent's answer now, and how many have ended: see [usingLink]. Counting the ends
     * makes each one a change of the value, which [watchForeground] sees even where it missed the call's beginning.
     */
    private data class Uses(val now: Int, val ended: Long)

    private val inUse = MutableStateFlow(Uses(0, 0))

    /**
     * Whether something uses the link: see [watchForeground]. Every change is a value, as [inUse] counts the ends of
     * the calls that wait.
     */
    private val busyNow = combine(jobState, logPullState, inUse) { job, pull, using ->
        job?.needsPhone == true || pull is LogPullState.Pulling || using.now > 0
    }

    /**
     * The head unit whose link was ended when the app left the foreground, which the app connects to again when it
     * comes back: see [leave] and [comeBack]. Null when there is none.
     */
    @Volatile
    private var remembered: String? = null

    /** The connects under way, the user's and the app's own on coming back: leaving the app gives them up. */
    private val dialling: MutableSet<Job> = ConcurrentHashMap.newKeySet()

    /** The app's own connect on coming back, while it runs: what the user asks for takes its place. */
    @Volatile
    private var returning: Job? = null

    /** The link the car has been asked to end ([resetSerial]): its end is the app's to follow, not a failure. */
    @Volatile
    private var resetting: AgentSession? = null

    /**
     * The user disconnected: the app takes no link the car opens until they connect, or the app comes back to the
     * foreground. Otherwise the car would have it connected again within half a minute.
     */
    private val heldBack = MutableStateFlow(false)

    /** How many [attempt]s run. One listens for the car by itself, so the app does not listen beside it. */
    private val attempting = MutableStateFlow(0)

    /** The `serial` object of each head unit's last `info.get` in this run, by this app's id: see [agentSerial]. */
    private val serialStatus = ConcurrentHashMap<String, JsonObject>()

    init {
        scope.launch { watchForeground() }
        scope.launch { watchIncoming() }
    }

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
     * it: no link stays open, and the state is [ConnectionState.Disconnected]. So does the app leaving the
     * foreground ([leave]), which fails the connect instead of cancelling its caller.
     *
     * [byUser] says that the user started it, as with a button: its knocks may then go through the car's Serial Port
     * service once in each burst ([KnockFallback.ONCE_PER_BURST]), where one the app makes by itself, as any caller that
     * does not say, may once in the whole knock ([KnockFallback.ONCE_PER_KNOCK]).
     */
    suspend fun connect(headUnitId: String, byUser: Boolean = false): Result<Unit> =
        connect(headUnitId, fromPhone = false, if (byUser) KnockFallback.ONCE_PER_BURST else KnockFallback.ONCE_PER_KNOCK)

    /**
     * [connect], once, the way a head unit whose agent leaves the phone's own links alone ([HeadUnit.phoneOpenedLeft]),
     * or dials when knocked ([HeadUnit.serialKnock]), is not connected to: the phone dials the car, with the link's magic
     * bytes and `hello`, as well as listening for it. For the user to ask for where a connect that only listened or
     * knocked had no link ([ConnectionState.Failed.listenedOnly]); nothing else does it.
     */
    suspend fun connectFromPhone(headUnitId: String): Result<Unit> = connect(headUnitId, fromPhone = true, KnockFallback.ONCE_PER_BURST)

    private suspend fun connect(headUnitId: String, fromPhone: Boolean, knockFallback: KnockFallback): Result<Unit> {
        heldBack.value = false
        // What the user asks for takes the place of what the app was waiting to do by itself.
        remembered = null
        returning?.cancel()
        stopRejoining()
        return dialled {
            connectLock.withLock {
                // A wait that began while this one waited for the lock was not there to be ended above.
                stopRejoining()
                attempt(headUnitId, quiet = false, fromPhone = fromPhone, knockFallback = knockFallback)
            }
        } ?: Result.failure(IOException(LEFT_WHILE_CONNECTING))
    }

    /**
     * Runs [block], a connect, so that the app leaving the foreground can give it up ([leave]): null where it did.
     * A caller that is cancelled is cancelled as it was before.
     */
    private suspend fun <T> dialled(block: suspend () -> T): T? = coroutineScope {
        // Known to [leave] before it begins: begun at once, it could reach the link and wait there, the lock held, before
        // it was added, and a leave that came then would miss it and wait for the lock behind it.
        val work = async(start = CoroutineStart.LAZY) { block() }
        dialling += work
        try {
            work.await()
        } catch (e: CancellationException) {
            // Only the connect was cancelled, not its caller: the app left.
            ensureActive()
            null
        } finally {
            dialling -= work
        }
    }

    /**
     * What [connect] does, with [connectLock] held. When [quiet], it is one of the app's own tries, after the agent
     * restarted or on coming back to the foreground: a failure is the caller's to publish, and the state stays
     * [ConnectionState.Connecting]. With [fromPhone], the phone dials the car where it would otherwise only listen or
     * knock ([connectFromPhone]). [knockBursts] and [knockFallback] are as for [attemptOver].
     */
    private suspend fun attempt(
        headUnitId: String,
        quiet: Boolean,
        incoming: IncomingLink? = null,
        fromPhone: Boolean = false,
        knockBursts: Int? = null,
        knockFallback: KnockFallback = KnockFallback.ONCE_PER_KNOCK,
    ): Result<Unit> {
        attempting.update { it + 1 }
        // A link the car opened, until it is the first link greeted. One that is never used is closed.
        var spare: Link? = incoming?.link
        try {
            return attemptOver(headUnitId, quiet, incoming?.endpoint, fromPhone, knockBursts, knockFallback) { spare.also { spare = null } }
        } finally {
            spare?.close()
            attempting.update { it - 1 }
        }
    }

    /**
     * What [attempt] does. Where [only] is given, that endpoint alone is tried, and its first link is the one
     * [given] gives, if any: a link the car opened. [fromPhone] is as for [attempt]. [knockBursts]: how many bursts a
     * knock makes, to a head unit whose agent dials when knocked ([HeadUnit.serialKnock]), where not the connector's own;
     * [knockFallback]: how often its knocks may go through the car's Serial Port service ([LinkConnector.knock]), the
     * same for the knocks of every link the connect opens.
     */
    private suspend fun attemptOver(
        headUnitId: String,
        quiet: Boolean,
        only: Endpoint?,
        fromPhone: Boolean,
        knockBursts: Int?,
        knockFallback: KnockFallback,
        given: () -> Link?,
    ): Result<Unit> {
        drop("The app connected again")
        val saved = headUnits.headUnit(headUnitId).first()
            ?: return Result.failure(IOException("That head unit is no longer saved"))
        // For this connect only: what is saved is changed by the next `hello` and `info.get` alone. The phone's own
        // link, which neither a knock nor a connect that only listens makes.
        val unit = if (fromPhone) saved.copy(phoneOpenedLeft = false, serialKnock = false) else saved
        connectionState.value = ConnectionState.Connecting(headUnitId)
        val failures = ArrayList<Pair<Endpoint, String>>()
        // A connect that only listened, and had no link from the car: see [ConnectionState.Failed.listenedOnly].
        var listenedOnly = false
        var opened: AgentSession? = null
        try {
            val endpoints = if (only != null) listOf(only) else unit.endpoints.filter(connector::supports).inConnectOrder()
            for (endpoint in endpoints) {
                opened = null
                val reason = try {
                    val paired = dial(unit, endpoint, given(), knockBursts, knockFallback) { opened = it }
                    return if (paired) Result.success(Unit) else Result.failure(PairingNeeded())
                } catch (e: RemoteException) {
                    AgentText.request(e.code)
                } catch (e: IOException) {
                    if (e is NoCarLinkException) listenedOnly = true
                    if (e is KnockUnansweredException) knockFailure(endpoint, e) else e.message ?: "The head unit couldn't be reached"
                }
                opened?.let { abandon(it, reason) }
                // A link that failed after it was shown as connected, as one whose `job.status` got no answer, is not.
                connectionState.value = ConnectionState.Connecting(headUnitId)
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
        if (!quiet) connectionState.value = ConnectionState.Failed(headUnitId, reason, listenedOnly)
        return Result.failure(if (listenedOnly) NoCarLinkException(reason) else IOException(reason))
    }

    /**
     * What the user is told where a connect knocked at [endpoint] and the car's link did not come ([failure]). The
     * phone is connected to the car in some way where the car holds it over hands-free or audio ([carsHolding]), or
     * where the knock's connects reached the car (it did not stop as [KnockUnansweredException.carAway]). Then, where the
     * phone says that the car has no media audio connection to it, that is the likely cause: the car turns the phone's
     * requests down, and no knock reaches the agent. Android also says there is none while the car is off or out of
     * range, so that is not said where the phone has no connection to the car: a knock that did not reach it says that
     * the car is not in reach. The phone's own connect is offered after the other two as the last thing to try; the
     * failure offers it in every case, as any connect that only listened does ([ConnectionState.Failed.listenedOnly]).
     */
    private fun knockFailure(endpoint: Endpoint, failure: KnockUnansweredException): String {
        val serial = endpoint as? SerialEndpoint
        val holding = serial != null && serial.address.uppercase() in carsHolding.value
        val audio = serial?.let {
            try {
                mediaAudio(it.address)
            } catch (e: RuntimeException) {
                null
            }
        }
        val connected = holding || !failure.carAway
        return when {
            connected && audio == false -> KNOCK_NO_MEDIA_AUDIO + " " + KNOCK_FROM_PHONE
            !connected -> KNOCK_NOT_IN_REACH
            else -> KNOCK_NO_LINK + " " + KNOCK_FROM_PHONE
        }
    }

    /**
     * Follows whether the app is in front of the user and whether anything uses the link, for as long as the
     * repository lives. Busy means a job that needs the phone ([JobView.needsPhone], which covers the app's own wait
     * to connect again after the agent restarted), a pull of logs that runs, a call that waits for the agent's
     * answer ([usingLink]), or an open terminal ([usingSession]).
     *
     * Out of sight and not busy, the link goes ([leave]): at once when the app leaves, or once what kept it busy is
     * over if the app is still out of sight then. Back in sight, the app connects again to the head unit it let go
     * of ([comeBack]). The foreground service a job runs under is not this one's: it keeps the process for the job.
     */
    private suspend fun watchForeground() {
        var shownBefore = false
        // Every change is looked at, not only a change of the two answers: a leave that found the link busy after all,
        // once it held the lock, is tried again on the next change, which the end of that use is.
        combine(foreground.inForeground, busyNow) { shown, busy -> shown to busy }.collect { (shown, busy) ->
            if (shown && !shownBefore) {
                // Coming back undoes a disconnect of the user's, as far as the car's links go: see [heldBack].
                heldBack.value = false
                comeBack()
            }
            if (!shown && !busy) leave()
            shownBefore = shown
        }
    }

    /**
     * Listens for a link the car opens to the phone, for as long as the repository lives, whenever the app would want
     * a link and has none: it is in front of the user or busy, Bluetooth is on, the user has not disconnected
     * ([heldBack]), there is neither a link nor an [attempt], and a saved head unit has an endpoint the connector can
     * take a link from. Each link is taken by [take], one at a time; listening that cannot be had waits for a change
     * of any of these, as Bluetooth being switched on again.
     *
     * The app's own wait to connect again after the agent restarted ([rejoin]) is no attempt, so the car's link can
     * come first and is then the one the job is followed over.
     */
    private suspend fun watchIncoming() {
        val wanted = combine(foreground.inForeground, busyNow, heldBack, bluetoothOn) { shown, busy, held, on -> (shown || busy) && !held && on }
        val free = combine(connectionState, attempting) { state, attempts ->
            attempts == 0 && state !is ConnectionState.Connected && state !is ConnectionState.NeedsPairing
        }
        combine(wanted, free, headUnits.headUnits) { want, isFree, units ->
            if (want && isFree) units.flatMap { it.endpoints }.filter(connector::accepts).distinct() else emptyList()
        }.distinctUntilChanged().collectLatest { endpoints ->
            if (endpoints.isEmpty()) return@collectLatest
            while (true) {
                val incoming = try {
                    connector.accept(endpoints)
                } catch (e: IOException) {
                    // Bluetooth went off, or the phone will not listen: until something changes.
                    return@collectLatest
                }
                // On the app's scope: what it does changes what is followed here, which would cancel it. Leaving the
                // app gives it up, as it does a connect.
                val work = scope.launch(start = CoroutineStart.LAZY) { take(incoming) }
                dialling += work
                work.invokeOnCompletion { dialling -= work }
                work.start()
                work.join()
            }
        }
    }

    /**
     * Takes a link the car opened as a connect of the app's own to the head unit whose endpoint it came from: `hello`,
     * `auth`, and the state that follows, or the wait to be paired. Where the app no longer wants it, as when a link
     * came meanwhile or the user disconnected, it is closed. A failure is quiet: the state is left as it was before, and
     * listening goes on. One that succeeds while the app waited to connect again after the agent restarted is the
     * reconnect, and the wait ends; where it is another head unit's link, the job waited for is one whose link was lost.
     */
    private suspend fun take(incoming: IncomingLink) {
        var spare: Link? = incoming.link
        try {
            connectLock.withLock {
                val unit = headUnits.headUnits.first().firstOrNull { incoming.endpoint in it.endpoints } ?: return@withLock
                val wanted = !heldBack.value && (foreground.inForeground.value || busy())
                if (!wanted || session != null || unpaired != null) return@withLock
                val before = connectionState.value
                val link = spare.also { spare = null }
                val result = attempt(unit.id, quiet = true, IncomingLink(incoming.endpoint, requireNotNull(link)), knockFallback = KnockFallback.ONCE_PER_KNOCK)
                log.line(
                    when {
                        result.isSuccess -> "the car's link is connected"
                        result.exceptionOrNull() is PairingNeeded -> "the car's link waits for the phone to be paired"
                        else -> "the car's link failed, quietly: ${result.exceptionOrNull()?.message}"
                    },
                )
                when {
                    result.isSuccess -> {
                        remembered = null
                        // The job is followed over this link already, where it is this head unit's.
                        rejoining.getAndSet(null)?.cancel()
                        lostElsewhere(unit.id)
                    }
                    result.exceptionOrNull() is PairingNeeded -> {
                        remembered = null
                        rejoining.getAndSet(null)?.cancel()
                        lost(unit.id)
                        lostElsewhere(unit.id)
                    }
                    connectionState.value == ConnectionState.Connecting(unit.id) -> connectionState.value = before
                }
            }
        } finally {
            spare?.close()
        }
    }

    /** Whether something uses the link now: see [watchForeground]. */
    private fun busy(): Boolean = jobState.value?.needsPhone == true || logPullState.value is LogPullState.Pulling || inUse.value.now > 0

    /** The head unit [state] has a link to, or is connecting to. */
    private fun linkedTo(state: ConnectionState): String? = when (state) {
        is ConnectionState.Connected -> state.headUnitId
        is ConnectionState.Connecting -> state.headUnitId
        is ConnectionState.NeedsPairing -> state.headUnitId
        else -> null
    }

    /**
     * Ends the link from this side, as the app leaves the foreground with nothing using it: a connect under way is
     * given up, a link that waits to be paired goes too, the state is [ConnectionState.Disconnected], and the head
     * unit is remembered for [comeBack]. Nothing is done where something has come to use the link meanwhile, as a
     * job found running on a link just made.
     */
    private suspend fun leave() {
        val before = linkedTo(connectionState.value)
        // Also one that waits for the lock, and so shows nothing yet.
        dialling.forEach { it.cancel() }
        if (before == null) return
        stopRejoining()
        connectLock.withLock {
            stopRejoining()
            if (busy()) return@withLock
            val target = linkedTo(connectionState.value) ?: before
            drop(LEFT)
            connectionState.value = ConnectionState.Disconnected
            remembered = target
        }
    }

    /**
     * Connects again, quietly, to the head unit whose link was ended when the app left the foreground: one try,
     * shown as [ConnectionState.Connecting]. A failure leaves [ConnectionState.Disconnected] and shows nothing, but
     * for one that only listened while the car holds the phone ([quietFailure]); a phone that has to be paired ends in
     * [ConnectionState.NeedsPairing], as any connect does. A head unit that is no longer saved is forgotten. The
     * user's [connect] and [disconnect] take its place.
     */
    private fun comeBack() {
        val headUnitId = remembered ?: return
        val work = scope.launch(start = CoroutineStart.LAZY) {
            connectLock.withLock {
                currentCoroutineContext().ensureActive()
                if (remembered != headUnitId) return@withLock
                remembered = null
                quietFailure(headUnitId, attempt(headUnitId, quiet = true, knockFallback = KnockFallback.ONCE_PER_KNOCK))
            }
        }
        returning = work
        dialling += work
        work.invokeOnCompletion {
            dialling -= work
            if (returning === work) returning = null
        }
        work.start()
    }

    /**
     * What one of the app's own tries to [headUnitId] that it does not publish itself ([comeBack], and the connect
     * after [resetSerial]) comes to: a failure leaves [ConnectionState.Disconnected] and shows nothing, so that the app
     * coming to the foreground away from the car does not show one each time; a phone that has to be paired is left in
     * [ConnectionState.NeedsPairing]. But a try that only listened for the car's link and had none
     * ([NoCarLinkException]) while that car holds the phone over hands-free or audio ([carsHolding]) is shown as failed
     * with the offer to connect from the phone ([ConnectionState.Failed.listenedOnly]): the user is likely in the car
     * then, and would otherwise see nothing come of the wait, and wait as long again on the next connect before the
     * offer.
     */
    private suspend fun quietFailure(headUnitId: String, result: Result<Unit>) {
        val failure = result.exceptionOrNull() ?: return
        if (failure is PairingNeeded) return
        connectionState.value = if (failure is NoCarLinkException && carHolds(headUnitId)) {
            ConnectionState.Failed(headUnitId, failure.message ?: "The car didn't connect to this app", listenedOnly = true)
        } else {
            ConnectionState.Disconnected
        }
    }

    /** Whether the car of a Bluetooth serial endpoint of [headUnitId]'s holds the phone now: see [carsHolding]. */
    private suspend fun carHolds(headUnitId: String): Boolean {
        val unit = headUnits.headUnit(headUnitId).first() ?: return false
        val holding = carsHolding.value
        return unit.endpoints.any { it is SerialEndpoint && it.address.uppercase() in holding }
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
     *
     * To a head unit whose agent dials only when knocked ([HeadUnit.serialKnock]), each connect is one burst of
     * knocks, one every [Rejoin.knockEveryMs], until [Rejoin.knockGiveUpAfterMs]; a link the car opens in between is
     * taken as at any other time ([take]), which ends the wait. The job is then shown as one whose wait is over
     * ([JobView.waitedOut]), so that the screen says so and what to do. None of these knocks goes through the car's
     * Serial Port service ([KnockFallback.NONE]): while the agent restarts, a link that came up there
     * would reach another program at the car, and the agent on trial dials the phone by itself, which the knock listens
     * for all the while.
     */
    private suspend fun rejoin(headUnitId: String) {
        val began = clock()
        delay(rejoin.firstAfterMs)
        var failure: String
        // The last try only listened, and the car's link did not come: see [ConnectionState.Failed.listenedOnly].
        var listenedOnly: Boolean
        while (true) {
            // As saved now: a `hello` since the restart may have changed it.
            val knocks = headUnits.headUnit(headUnitId).first()?.serialKnock == true
            val tried = clock()
            val result = connectLock.withLock {
                // Taking a free lock asks nothing of the caller: one that was told to stop must not connect.
                currentCoroutineContext().ensureActive()
                attempt(headUnitId, quiet = true, knockBursts = if (knocks) 1 else null, knockFallback = KnockFallback.NONE)
            }
            if (result.isSuccess) return
            if (result.exceptionOrNull() is PairingNeeded) return lost(headUnitId)
            failure = result.exceptionOrNull()?.message.orEmpty()
            listenedOnly = result.exceptionOrNull() is NoCarLinkException
            val pause = if (knocks) (rejoin.knockEveryMs - (clock() - tried)).coerceAtLeast(0) else rejoin.pauseMs
            val giveUpAfter = if (knocks) rejoin.knockGiveUpAfterMs else rejoin.giveUpAfterMs
            if (clock() - began + pause >= giveUpAfter) break
            delay(pause)
        }
        connectLock.withLock {
            currentCoroutineContext().ensureActive()
            lost(headUnitId, waitedOut = true)
            connectionState.value =
                ConnectionState.Failed(headUnitId, "The head unit's agent hasn't answered since it restarted. $failure".trim(), listenedOnly)
        }
    }

    /** The job the app was connecting again for, if any, is one whose link was lost; with [waitedOut], as [rejoin] gave up. */
    private fun lost(headUnitId: String, waitedOut: Boolean = false) = jobState.update { view ->
        if (view != null && view.reconnecting && view.headUnitId == headUnitId) {
            view.copy(reconnecting = false, linkLost = true, waitedOut = waitedOut)
        } else {
            view
        }
    }

    /**
     * The job the app was connecting again for on another head unit than [headUnitId], if any, is one whose link was
     * lost: the link is now to [headUnitId], and the wait for the other was ended with nothing to follow its job over.
     */
    private fun lostElsewhere(headUnitId: String) = jobState.update { view ->
        if (view != null && view.reconnecting && view.headUnitId != headUnitId) view.copy(reconnecting = false, linkLost = true) else view
    }

    /**
     * Ends the app's own wait to connect again, if there is one. The wait is begun with [connectLock] held, so
     * only a caller that holds it knows there is none; one that does not hold it ends a try that is under way.
     */
    private fun stopRejoining() {
        rejoining.getAndSet(null)?.cancel()
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
    suspend fun pair(code: String): Result<Unit> = using { pairOver(code) }

    /** What [pair] does, while it counts as a use of the link. */
    private suspend fun pairOver(code: String): Result<Unit> = connectLock.withLock {
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

    /**
     * Ends the link. A job that is fetching pauses on the head unit; one that is installing finishes there. The app
     * does not connect again by itself when it comes back to the foreground.
     */
    suspend fun disconnect() {
        heldBack.value = true
        remembered = null
        returning?.cancel()
        stopRejoining()
        connectLock.withLock {
            // As in [connect]: a wait that began meanwhile ends here.
            stopRejoining()
            drop("Disconnected")
            connectionState.value = ConnectionState.Disconnected
        }
    }

    /**
     * How the agent of the head unit [headUnitId] said its Bluetooth serial stood, in its last `info.get` in this run:
     * its `serial` object as it came, for the diagnostics the user copies. Null where the app had no link to it in
     * this run, or its agent has no such object.
     */
    fun agentSerial(headUnitId: String): JsonObject? = serialStatus[headUnitId]

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
        if (session === current) show(connected.headUnitId, connected.endpoint, info, installed, autostart, current.client.capabilities)
    }

    /**
     * Switches on or off whether the agent is started with the head unit, on an agent that has the switch
     * ([ConnectionState.Connected.autostart]). What the agent answers is how things then stand, and is published.
     *
     * Where the agent refuses, the failure is an [AgentRefusal] in the app's words ([AgentText.autostart]), and
     * how things stand is read again: a refusal means that they are not as the app had them.
     */
    suspend fun setAutostart(on: Boolean): Result<Unit> = using {
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

    /**
     * What the agent's compatibility checks found on the connected head unit: what is kept with it, or, where
     * nothing is kept yet and its agent lists [ProtocolConstants.CAPABILITY_CHECKS], what they find now, which is
     * then kept. So they run by themselves once for a head unit, on the first link on which they can be answered;
     * after that only by hand ([recheck]).
     *
     * Null where nothing is kept and they cannot be run now: no link, an agent without them, a job that runs (the
     * agent answers `busy` then), or a run that failed. Nothing is kept then, and the next link tries again.
     */
    suspend fun ensureChecked(): HeadUnitChecks? = using {
        checksLock.withLock {
            val current = session ?: return@withLock null
            val connected = connectionState.value as? ConnectionState.Connected ?: return@withLock null
            headUnits.headUnit(connected.headUnitId).first()?.checks?.let { return@withLock it }
            if (!connected.checks || jobRunsOn(connected.headUnitId)) return@withLock null
            try {
                runAll(current, connected.headUnitId)
            } catch (e: RemoteException) {
                null
            } catch (e: IOException) {
                null
            }
        }
    }

    /**
     * Runs the checks on the connected head unit again, by hand, and keeps what they find in place of what was
     * kept. A problem the user put aside that is still there is shown again.
     */
    suspend fun recheck(): Result<HeadUnitChecks> = checksLock.withLock {
        checksSession { current, connected -> runAll(current, connected.headUnitId) }
    }

    /**
     * Runs the check [id] on the connected head unit now, and returns it whole, with the token of the list it
     * found: what a fix is confirmed against. What is kept for it is brought up to date.
     */
    suspend fun runCheck(id: String): Result<Check> = checksLock.withLock {
        checksSession { current, connected ->
            val check = current.client.checksRun(id).single()
            if (session === current) keepCheck(connected.headUnitId, check)
            check
        }
    }

    /**
     * Runs the fix of the check [id] with the [token] of the list the user confirmed. What is kept is brought up
     * to date: a check that finds nothing now is dropped, and one that still finds something is run again, so its
     * new list is kept. Then what the head unit holds is read again, since a fix leaves a restart needed and may
     * change what the agent says of starting with the car.
     *
     * A refusal is an [AgentRefusal] in the app's words ([AgentText.check]). With `stale` the list is no longer
     * the one the token is of: the caller runs the check again and shows the user the new list.
     */
    suspend fun fixCheck(id: String, token: String): Result<CheckFixResult> = using {
        val result = checksLock.withLock {
            checksSession { current, connected ->
                val outcome = current.client.checksFix(id, token)
                if (outcome.state == CheckState.PROBLEM) {
                    // The agent's answer holds only what fits its frame: the list is read again, whole.
                    val again = try {
                        current.client.checksRun(id).single()
                    } catch (e: RemoteException) {
                        null
                    }
                    if (again != null && session === current) keepCheck(connected.headUnitId, again)
                } else if (session === current) {
                    dropCheck(connected.headUnitId, id)
                }
                outcome
            }
        }
        if (result.isSuccess) refresh()
        return result
    }

    /** Puts aside the problem of the check [id] on the head unit [headUnitId]: it is no longer shown, until [recheck]. */
    suspend fun dismissCheck(headUnitId: String, id: String) {
        headUnits.update(headUnitId) { unit ->
            val kept = unit.checks ?: return@update unit
            unit.copy(checks = kept.copy(problems = kept.problems.map { if (it.id == id) it.copy(dismissed = true) else it }))
        }
    }

    /** A job on [headUnitId] is running, or being followed: the checks wait for it. */
    private fun jobRunsOn(headUnitId: String): Boolean = jobState.value?.let { it.headUnitId == headUnitId && !it.ended } == true

    /** Runs every check over [current] and keeps what they find with [headUnitId], none of it put aside. */
    private suspend fun runAll(current: AgentSession, headUnitId: String): HeadUnitChecks {
        val checks = HeadUnitChecks(now(), current.client.checksRun().mapNotNull { CheckProblem.of(it) })
        if (session === current) headUnits.update(headUnitId) { it.copy(checks = checks) }
        return checks
    }

    /** Keeps [check] as it was found now, with whether it was put aside; a check that found nothing is dropped. */
    private suspend fun keepCheck(headUnitId: String, check: Check) = headUnits.update(headUnitId) { unit ->
        val kept = unit.checks ?: HeadUnitChecks(now())
        val was = kept.problem(check.id)
        val problem = CheckProblem.of(check, dismissed = was?.dismissed == true)
        val problems = when {
            problem == null -> kept.problems.filter { it.id != check.id }
            was == null -> kept.problems + problem
            else -> kept.problems.map { if (it.id == check.id) problem else it }
        }
        unit.copy(checks = kept.copy(problems = problems))
    }

    private suspend fun dropCheck(headUnitId: String, id: String) = headUnits.update(headUnitId) { unit ->
        val kept = unit.checks ?: return@update unit
        unit.copy(checks = kept.copy(problems = kept.problems.filter { it.id != id }))
    }

    /**
     * [block] over the link to the connected head unit, on an agent that has the checks. A refusal comes out in
     * the words of [AgentText.check].
     */
    private suspend fun <T> checksSession(block: suspend (AgentSession, ConnectionState.Connected) -> T): Result<T> = using {
        val current = session ?: return Result.failure(IOException(NOT_CONNECTED))
        val connected = connectionState.value as? ConnectionState.Connected ?: return Result.failure(IOException(NOT_CONNECTED))
        if (!connected.checks) return Result.failure(IOException(NO_CHECKS))
        return try {
            Result.success(block(current, connected))
        } catch (e: RemoteException) {
            Result.failure(AgentRefusal(e.code, AgentText.check(e.code)))
        } catch (e: IOException) {
            Result.failure(e)
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

    /**
     * The log files of the package [packageId] on the connected head unit, newest first: of the agent's own for
     * [AGENT_PACKAGE_ID]. Only an agent that lists [ProtocolConstants.CAPABILITY_FILES] has them.
     */
    suspend fun listLogs(packageId: String): Result<FileListing> = withSession { current ->
        if (ProtocolConstants.CAPABILITY_FILES !in current.client.capabilities) throw IOException(NO_FILES)
        try {
            current.client.filesList(packageId)
        } catch (e: RemoteException) {
            if (e.code == "busy") throw IOException(PULL_BUSY)
            throw e
        }
    }

    /**
     * Pulls the log files [request] names into a ZIP, as [pullLogs] does, on the app's own scope: it goes on when
     * the screen that started it is left, and the foreground service keeps the process alive meanwhile. Where
     * stands is [logPull]. A pull that runs already is left to run, and this does nothing.
     */
    fun startPull(request: LogPullRequest, appVersion: String) {
        if (pulling?.isActive == true) return
        // In the state at once, so a screen that started it never sees the last one's end.
        logPullState.value = LogPullState.Pulling(request.packageId, 1, request.files.size, 0, request.files.sumOf { LogPull.wanted(it, request.tailBytes) })
        pulling = scope.launch { pullLogs(request, appVersion) }
    }

    /** Stops the pull that runs: the stream that is open is stopped with `files.cancel`, and no ZIP is left. */
    fun cancelPull() {
        pulling?.cancel()
    }

    /**
     * Pulls the log files [request] names from the connected head unit into a ZIP in the share folder, and
     * returns it: see [LogPull]. The share folder is emptied first. Where it stands is published in [logPull]
     * throughout, and so is its end, whether it went well or not.
     *
     * While a job runs the agent sends no files, and the failure says to wait for it ([AgentRefusal] `busy`).
     */
    suspend fun pullLogs(request: LogPullRequest, appVersion: String): Result<File> {
        val id = request.packageId
        val total = request.files.sumOf { LogPull.wanted(it, request.tailBytes) }
        logPullState.value = LogPullState.Pulling(id, 1, request.files.size, 0, total)
        keeper.jobRunning()
        val result = try {
            withSession { current ->
                val connected = connectionState.value as? ConnectionState.Connected ?: throw IOException(NOT_CONNECTED)
                if (!connected.files) throw IOException(NO_FILES)
                val began = now()
                val about = LogPullAbout(
                    packageName = connected.installed.firstOrNull { it.id == id }?.name
                        ?: if (id == AGENT_PACKAGE_ID) "the agent" else id,
                    packageVersion = connected.installed.firstOrNull { it.id == id }?.version
                        ?: connected.info.agentVersion.takeIf { id == AGENT_PACKAGE_ID },
                    headUnitName = headUnits.headUnit(connected.headUnitId).first()?.name.orEmpty(),
                    agentVersion = connected.info.agentVersion,
                    appVersion = appVersion,
                    pulledAt = began,
                )
                // Only the app's own files are in the folder, and only those of the last pull.
                shareDir.listFiles()?.forEach { it.deleteRecursively() }
                shareDir.mkdirs()
                val zip = File(shareDir, "$id-${ZIP_TIME.format(Instant.ofEpochMilli(began).atZone(ZoneId.systemDefault()))}.zip")
                val files = LogPull(current.client, File(shareDir, "work")).run(request, about, zip) { file, bytes, all ->
                    logPullState.value = LogPullState.Pulling(id, file, request.files.size, bytes, all)
                }
                File(shareDir, "work").deleteRecursively()
                logPullState.value = LogPullState.Done(id, zip, files)
                zip
            }
        } catch (e: CancellationException) {
            logPullState.value = LogPullState.Cancelled(id)
            throw e
        }
        result.exceptionOrNull()?.let { failure ->
            val reason = if (failure is AgentRefusal && failure.code == "busy") PULL_BUSY else failure.message ?: "The logs couldn't be pulled"
            logPullState.value = LogPullState.Failed(id, reason)
        }
        return result
    }

    /**
     * Every option of the settings of the package [packageId] on the head unit [headUnitId], in the order of its manifest,
     * read page by page as the agent sends them, with how each stands in its settings file now. Only an agent that
     * lists [ProtocolConstants.CAPABILITY_SETTINGS] has them.
     *
     * A failure is a [SettingsException], whose [SettingsFailure] says why: [SettingsFailure.Busy] while an install
     * step runs, for one.
     */
    suspend fun loadSettings(headUnitId: String, packageId: String): Result<List<SettingEntry>> = settingsSession(headUnitId) { current, _ ->
        val all = ArrayList<SettingEntry>()
        var from: Int? = null
        do {
            val page = current.client.settingsGet(packageId, from)
            all += page.options
            val next = page.next
            // A page that brings nothing or goes back, or more options than a manifest has, would never end.
            if (next != null && (page.options.isEmpty() || next <= (from ?: 0) || all.size >= ManifestParser.MAX_SETTINGS)) {
                throw ProtocolException("The head unit sent an answer this app can't read")
            }
            from = next
        } while (next != null)
        all
    }

    /**
     * Saves [changes] to the settings of the package [packageId] on the head unit [headUnitId]: in one `settings.set`
     * where they fit one frame of the link, and otherwise in as few as they take, one after another
     * ([SettingsChanges.parts]). The agent takes each whole or not at all. A part that fails ends the save, and its
     * [SettingsException] names in [SettingsException.saved] the options of the parts before it, which are saved.
     *
     * Nothing is sent while a job runs on that head unit, which the agent changes no settings during. Once begun, the
     * parts go on to the end or to the first failure even where the caller is cancelled, so that what was saved is
     * known.
     */
    suspend fun saveSettings(headUnitId: String, packageId: String, changes: SettingsChanges): Result<Unit> = settingsSession(headUnitId) { current, connected ->
        if (jobRunsOn(connected.headUnitId)) throw SettingsException(SettingsFailure.Busy)
        val parts = try {
            changes.parts(packageId, current.peer.maxFrame)
        } catch (e: IllegalArgumentException) {
            throw SettingsException(SettingsFailure.Failed("A change is too large to send to the head unit"))
        }
        val saved = HashSet<String>()
        withContext(NonCancellable) {
            for (part in parts) {
                try {
                    current.client.settingsSet(packageId, part.set, part.reset)
                } catch (e: RemoteException) {
                    throw SettingsException(settingsFailure(e), saved)
                } catch (e: IOException) {
                    throw SettingsException(settingsFailure(e), saved)
                }
                saved += part.keys
            }
        }
    }

    /**
     * [block] over the link to the head unit [headUnitId], on an agent that has settings. A failure comes out as a
     * [SettingsException]: [SettingsFailure.NotConnected] where the link is to another head unit, whose package of
     * the same id is not the one asked about.
     */
    private suspend fun <T> settingsSession(headUnitId: String, block: suspend (AgentSession, ConnectionState.Connected) -> T): Result<T> = using {
        val current = session
        val connected = (connectionState.value as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }
        if (current == null || connected == null) return Result.failure(SettingsException(SettingsFailure.NotConnected))
        if (!connected.settings) return Result.failure(SettingsException(SettingsFailure.TooOld))
        return try {
            Result.success(block(current, connected))
        } catch (e: SettingsException) {
            Result.failure(e)
        } catch (e: RemoteException) {
            Result.failure(SettingsException(settingsFailure(e)))
        } catch (e: IOException) {
            Result.failure(SettingsException(settingsFailure(e)))
        }
    }

    /** What the screen is told of a refusal of `settings.get` or `settings.set`, or of a link that failed meanwhile. */
    private fun settingsFailure(e: Exception): SettingsFailure = when (e) {
        is RemoteException -> when (e.code) {
            "unsupported" -> SettingsFailure.TooOld
            "busy" -> SettingsFailure.Busy
            "not-found" -> SettingsFailure.NotFound
            "invalid" -> SettingsFailure.Invalid(AgentClient.settingFaults(e.error))
            else -> SettingsFailure.Failed(AgentText.settings(e.code))
        }
        is ProtocolException -> SettingsFailure.Failed(e.message ?: "The head unit sent an answer this app can't read")
        else -> SettingsFailure.LinkLost(e.message ?: "The link to the head unit was lost")
    }

    /** Restarts iDrive ([kind] `hmi`) or the whole head unit (`reboot`). */
    suspend fun restart(kind: String): Result<Unit> = withSession { current -> current.client.restart(kind) }

    /**
     * Has the car end this phone's Bluetooth serial link (`serial.reset`), which the agent closing its end does not
     * do: for when the phone connects and nothing arrives, because bytes an earlier link left wait in the car's
     * Bluetooth. Only an agent that lists [ProtocolConstants.CAPABILITY_SERIAL_RESET] has it, and it is refused while
     * a job needs the phone.
     *
     * With `sent` the car ends the very link it was asked on, about 300 ms after the answer. The app waits up to
     * [resetWaitMs] for that, ends the link itself where the car did not, and connects again by itself, quietly, as
     * on coming back to the foreground: shown as [ConnectionState.Connecting], and a failure leaves
     * [ConnectionState.Disconnected], or the failure with the offer to connect from the phone ([quietFailure]). Not
     * where the user disconnected or connected meanwhile. With `armed` no serial link was up on the head unit, as over
     * another kind of link, and nothing more happens: the car ends the next one that opens within the time the answer
     * gives.
     *
     * A refusal is an [AgentRefusal] in the app's words ([AgentText.serialReset]).
     */
    suspend fun resetSerial(): Result<SerialResetOutcome> = using {
        val current = session ?: return Result.failure(IOException(NOT_CONNECTED))
        val connected = connectionState.value as? ConnectionState.Connected ?: return Result.failure(IOException(NOT_CONNECTED))
        if (!connected.serialReset) return Result.failure(IOException(NO_SERIAL_RESET))
        if (jobState.value?.needsPhone == true) return Result.failure(IOException(RESET_WHILE_JOB))
        // Before the answer: the link may end on its heels.
        resetting = current
        val answer = try {
            current.client.serialReset()
        } catch (e: RemoteException) {
            resetting = null
            return Result.failure(AgentRefusal(e.code, AgentText.serialReset(e.code)))
        } catch (e: IOException) {
            resetting = null
            return Result.failure(e)
        }
        if (answer.state == SerialResetState.ARMED) {
            resetting = null
            return Result.success(SerialResetOutcome.Armed(answer.forMs ?: ARMED_FOR_MS))
        }
        val endedByCar = withTimeoutOrNull(resetWaitMs) { current.ended.await() } != null
        val again = connectLock.withLock {
            // Whether [pump] has seen the end yet or not, the link is no longer the app's.
            if (session === current) {
                session = null
                current.close(RESET_BY_APP)
                connectionState.value = ConnectionState.Connecting(connected.headUnitId)
            }
            resetting = null
            // The user disconnected, or connected, meanwhile: theirs stands.
            if (connectionState.value != ConnectionState.Connecting(connected.headUnitId)) return@withLock false
            // For the car to take the last link down, as between the links a connect opens.
            delay(greeting.pauseMs)
            // The user's: the reset was theirs.
            val result = attempt(connected.headUnitId, quiet = true, knockFallback = KnockFallback.ONCE_PER_BURST)
            quietFailure(connected.headUnitId, result)
            result.isSuccess
        }
        Result.success(SerialResetOutcome.Sent(endedByCar, again))
    }

    /**
     * Opens a link to [endpoint] and says `hello` on it, or on [first], a link the car opened, where given: a link
     * opened after that is the phone's own. [holding] is told of each session while it is open and not yet the
     * caller's, and of null once it has been closed again.
     *
     * A Bluetooth serial link that comes up is not yet a link to the agent: the car gives it to whichever program
     * there reads it first, and the agent lets go a link it took too late to see how it opened. Such a link ends
     * before `hello` is answered, or stays silent. It is closed and another is opened, [Greeting.serialTries]
     * times in all, counted in [tries] with the links [dial] opened before for the same reason. An answer of any
     * kind, a refusal too, is the agent's and is not tried again.
     *
     * Where the agent said that a link this phone opens itself is left to another program ([HeadUnit.phoneOpenedLeft]),
     * the connector only listens for the car's link. Where that has changed since the app last heard, a link the phone
     * opens is never answered; so from the first wait for an answer on a link the phone opened to an agent that dials,
     * the phone listens for the car's links until the greeting is over ([CarLinks]): through every wait for an answer
     * and every pause between the links. One the car opens while a link of the phone's waits for its
     * answer takes that link's place ([helloOrCarLink]): the phone's link is closed and the greeting goes on over the
     * car's, which is not counted again among the tries. One that comes in a pause, or while a link of the car's own
     * waits for its answer, is the next round's link, and is counted as the link it stands for would have been: it
     * ends the pause, as there is no link of the phone's for the car to take down. So the tries stay as many as before.
     * Nothing is left listening, and no link of the car's open, however the greeting ends.
     *
     * To a head unit whose agent dials only when knocked ([HeadUnit.serialKnock]), each link is the car's that a knock
     * brings ([LinkConnector.knock], with [knockBursts] and [knockFallback]), and is greeted as any link the car opened is; [HeadUnit.phoneOpenedLeft]
     * decides nothing, and no link of the phone's own is made.
     */
    private suspend fun greet(
        unit: HeadUnit,
        endpoint: Endpoint,
        first: Link?,
        knockBursts: Int?,
        knockFallback: KnockFallback,
        holding: (AgentSession?) -> Unit,
        tries: Tries,
    ): Pair<AgentSession, Hello> =
        coroutineScope {
            val carLinks = CarLinks(endpoint, this)
            try {
                rounds(unit, endpoint, first, knockBursts, knockFallback, holding, tries, carLinks)
            } finally {
                // Nothing listens, and no link of the car's that came is left open, however the greeting ends. The
                // scope waits for the listening to have stopped.
                carLinks.stop()
            }
        }

    /** The rounds of [greet], each over one link, with [carLinks] listening from the first link of the phone's on. */
    private suspend fun rounds(
        unit: HeadUnit,
        endpoint: Endpoint,
        first: Link?,
        knockBursts: Int?,
        knockFallback: KnockFallback,
        holding: (AgentSession?) -> Unit,
        tries: Tries,
        carLinks: CarLinks,
    ): Pair<AgentSession, Hello> {
        // A link the car opened in the place of a silent one of the phone's, for the next round.
        var carLink: Link? = null
        try {
            while (true) {
                val replacing = carLink
                carLink = null
                val came = if (replacing == null) carLinks.take() else null
                if (came != null) log.line("the car's link came between two links of the phone's: it is used for the next")
                val link = replacing
                    ?: came
                    ?: first.takeIf { tries.opened == 0 }
                    ?: if (unit.serialKnock) {
                        connector.knock(endpoint, knockBursts, knockFallback)
                    } else {
                        connector.connect(endpoint, unit.serialDial, listenOnly = unit.serialDial && unit.phoneOpenedLeft)
                    }
                if (replacing == null) tries.opened++
                val next = AgentSession(link, scope, now = clock).also(holding)
                val last = next.kind != Link.SERIAL || tries.opened >= greeting.serialTries
                val watch = unit.serialDial && next.kind == Link.SERIAL && link !== first && !link.openedByCar && connector.accepts(endpoint)
                // From the first link of the phone's that waits for its answer, until the greeting is over.
                if (watch) carLinks.listen()
                val failure = try {
                    when (val got = withTimeoutOrNull(if (last) greeting.timeoutMs else greeting.retryTimeoutMs) { helloOrCarLink(next, carLinks.takeIf { watch }) }) {
                        is Greeted.Answer -> return next to got.hello
                        is Greeted.CarLink -> {
                            log.line("the car's link came while the phone's waited for an answer: the phone's is closed, and the car's is used")
                            carLink = got.link
                            abandon(next, CAR_LINK_TAKEN)
                            holding(null)
                            continue
                        }
                        null -> NO_ANSWER
                    }
                } catch (e: LinkClosedException) {
                    e.message ?: "The link to the head unit was lost"
                }
                // The words are the last link's; the ones before it failed in one of the same two ways.
                if (last) throw IOException(tries.said(failure))
                abandon(next, failure)
                holding(null)
                // For the car to take the last link down; a link the car opens meanwhile ends the pause.
                if (carLinks.listening) carLinks.awaitFor(greeting.pauseMs) else delay(greeting.pauseMs)
            }
        } finally {
            carLink?.close()
        }
    }

    /** What [helloOrCarLink] came to. */
    private sealed interface Greeted {
        class Answer(val hello: Hello) : Greeted

        /** A link the car opened to the phone, with the magic bytes written, which nobody else holds. */
        class CarLink(val link: Link) : Greeted
    }

    /**
     * The links the car at [endpoint] opens to the phone while [greet] runs: from [listen] until [stop], one wait in
     * [LinkConnector.accept] after another, in [within], so that the phone listens between the links as well as beside
     * them, with no break but the moment from a link handed over to the next wait. The car keeps one such link up at a
     * time: the latest that has come and that nobody has taken is held, and one it takes the place of is closed, as is
     * the one held at [stop]. Listening that cannot be had ends the waits, until the next [listen].
     */
    private inner class CarLinks(private val endpoint: Endpoint, private val within: CoroutineScope) {
        private val held = MutableStateFlow<Link?>(null)
        private val lock = Any()

        /** Set by [stop]: a link that comes after it is closed at once. Guarded by [lock]. */
        private var stopped = false

        private var waits: Job? = null

        /** Whether it listens now. */
        val listening: Boolean get() = waits?.isActive == true

        /** Starts listening, where it does not already. */
        fun listen() {
            if (listening) return
            waits = within.launch {
                while (true) {
                    val incoming = try {
                        connector.accept(listOf(endpoint))
                    } catch (e: IOException) {
                        // The phone cannot listen now: the answers are waited for alone.
                        return@launch
                    }
                    hold(incoming.link)
                }
            }
        }

        /** Keeps [link] for whoever takes the next; one held before it is closed. */
        fun hold(link: Link) {
            val before = synchronized(lock) { if (stopped) link else held.getAndUpdate { link } }
            before?.close()
        }

        /** The link held, if there is one, which is the caller's from here. */
        fun take(): Link? = held.getAndUpdate { null }

        /** Waits for a link to be held, and takes it. */
        suspend fun await(): Link {
            while (true) {
                held.first { it != null }
                take()?.let { return it }
            }
        }

        /** Waits [millis] at most for a link to be held, and leaves it held. */
        suspend fun awaitFor(millis: Long) {
            withTimeoutOrNull(millis) { held.first { it != null } }
        }

        /** Stops listening, and closes the link held, if there is one. */
        fun stop() {
            waits?.cancel()
            synchronized(lock) { stopped = true }
            take()?.close()
        }
    }

    /**
     * Says `hello` on [next] and waits for the answer; with [carLinks], also takes a link the car opens to the phone
     * meanwhile, or one that came before and is held, and whichever comes first is what this comes to. Listening that
     * cannot be had leaves the answer to be waited for alone. A link of the car's that came just as the caller was
     * cancelled is held in [carLinks], as one that came after the answer is: [greet] closes it, or its next round takes it.
     *
     * @throws LinkClosedException if [next] ends first.
     */
    private suspend fun helloOrCarLink(next: AgentSession, carLinks: CarLinks?): Greeted {
        if (carLinks == null) return Greeted.Answer(next.client.hello(identity.id(), identity.name))
        val won = CompletableDeferred<Greeted>()
        return coroutineScope {
            val asking = launch {
                try {
                    won.complete(Greeted.Answer(next.client.hello(identity.id(), identity.name)))
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    // A refusal too: what the caller makes of it is as without the watch.
                    won.completeExceptionally(e)
                }
            }
            val taking = launch {
                val link = carLinks.await()
                // The answer came first: held again, for [CarLinks.stop] to close.
                if (!won.complete(Greeted.CarLink(link))) carLinks.hold(link)
            }
            try {
                won.await()
            } catch (e: CancellationException) {
                // A link of the car's that came just as the wait gave up, as its time ran out, is held for the next
                // round to take; where the caller has given up the whole greeting, [CarLinks.stop] closes it.
                keepWhatWon(won, e, { (it as? Greeted.CarLink)?.link }, carLinks::hold)
                throw e
            } finally {
                asking.cancel()
                taking.cancel()
            }
        }
    }

    /** The links a connect has opened to one endpoint, of the [Greeting.serialTries] it may: see [greet] and [dial]. */
    private class Tries {
        var opened = 0

        /** [failure], the last link's, and how many links were tried where there was more than one. */
        fun said(failure: String) = if (opened <= 1) failure else "$failure ($opened links were tried)"
    }

    /**
     * [block], the request [method] that a connect makes, given [Greeting.answerMs] for its answer, where [limited]:
     * one that has none fails with [Unanswered], and the link log says which it was. The caller ends the link.
     */
    private suspend fun <T> answered(method: String, limited: Boolean = true, block: suspend () -> T): T {
        if (!limited) return block()
        val answer = withTimeoutOrNull(greeting.answerMs) { Answer(block()) }
        if (answer == null) {
            log.line("no answer to $method in ${greeting.answerMs} ms while connecting: the link is ended")
            throw Unanswered(method)
        }
        return answer.value
    }

    /** What [answered] waited for, which may itself be null. */
    private class Answer<T>(val value: T)

    /**
     * Opens a link to [unit] over [endpoint] and takes it as far as it goes: connected, which is true, or
     * waiting for the phone to be paired, which is false. [first] and [holding] are as for [greet].
     *
     * A key the agent no longer takes is of no use again: the agent forgot its phones, or holds another key for
     * this one. It is dropped, and since the agent ends a link on which `auth` failed, a new link is opened, on
     * which the phone has no key and is paired anew.
     *
     * A request after `hello` that gets no answer in [Greeting.answerMs] ends the link. On Bluetooth serial the link
     * may be one the agent no longer serves, as a `hello` with no answer may be, so another is opened as [greet] does,
     * within the same [Greeting.serialTries]; on any other link the connect fails.
     */
    private suspend fun dial(unit: HeadUnit, endpoint: Endpoint, first: Link?, knockBursts: Int?, knockFallback: KnockFallback, holding: (AgentSession?) -> Unit): Boolean {
        var stale = false
        var given = first
        var tries = Tries()
        while (true) {
            val (next, hello) = greet(unit, endpoint, given.also { given = null }, knockBursts, knockFallback, holding, tries)
            try {
                return open(unit, endpoint, next, hello, stale)
            } catch (e: StaleKey) {
                // Once: the second link is opened with no key to be stale, and has tries of its own.
                if (stale) throw e
                stale = true
                tries = Tries()
                keys.remove(requireNotNull(hello.headUnitId))
                abandon(next, e.message.orEmpty())
                holding(null)
                if (next.kind == Link.SERIAL) delay(greeting.pauseMs)
            } catch (e: Unanswered) {
                if (next.kind != Link.SERIAL || tries.opened >= greeting.serialTries) throw IOException(tries.said(NO_ANSWER))
                abandon(next, NO_ANSWER)
                holding(null)
                // It may have been shown as connected before the request that got no answer.
                connectionState.value = ConnectionState.Connecting(unit.id)
                delay(greeting.pauseMs)
            }
        }
    }

    /** What [dial] does with a link that has been greeted. */
    private suspend fun open(unit: HeadUnit, endpoint: Endpoint, next: AgentSession, hello: Hello, stale: Boolean): Boolean {
        if (unit.headUnitId != null && unit.headUnitId != hello.headUnitId) {
            throw ProtocolException("This isn't the head unit saved as ${unit.name}. Add it as a new one.")
        }
        // Kept from every `hello`, for how the next connect is made: see [HeadUnit.serialDial] and [HeadUnit.serialKnock].
        // From what is saved, not from [unit], which a connect from the phone changed for itself.
        val dials = ProtocolConstants.CAPABILITY_SERIAL_DIAL in hello.capabilities
        val knocks = ProtocolConstants.CAPABILITY_SERIAL_KNOCK in hello.capabilities
        headUnits.update(unit.id) { if (it.serialDial == dials && it.serialKnock == knocks) it else it.copy(serialDial = dials, serialKnock = knocks) }
        val agentId = requireNotNull(hello.headUnitId)
        if (keys.has(agentId)) answered("auth") { prove(next, agentId) }
        // The first thing asked also tells whether the agent takes this phone as it is.
        val info = try {
            answered("info.get") { next.client.info() }
        } catch (e: RemoteException) {
            if (e.code != "unauthenticated") throw e
            val waiting = Unpaired(next, unit, endpoint, hello)
            unpaired = waiting
            connectionState.value = ConnectionState.NeedsPairing(unit.id, endpoint, stale || forgottenBy == unit.id, hello.capabilities)
            scope.launch { attend(waiting) }
            return false
        }
        finish(unit, endpoint, next, hello, info, limited = true)
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
        val ended = waiting.session.ended.await()
        val failed = connectLock.withLock {
            if (unpaired !== waiting) return@withLock null
            unpaired = null
            ConnectionState.Failed(waiting.unit.id, endReasonNow(waiting.session, ended)).also { connectionState.value = it }
        }
        if (failed != null) settleEnd(waiting.session, ended, failed)
    }

    /**
     * Why [link] ended, as far as it can be told at once: [endReason] without its wait. The link's own [reason], or,
     * for a Bluetooth serial link that ended with the phone's Bluetooth off already, [BLUETOOTH_OFF].
     */
    private fun endReasonNow(link: AgentSession, reason: String): String = when {
        // What the agent said of why it ended the link stands.
        link.closing != null -> reason
        link.kind == Link.SERIAL && !bluetoothOn.value -> BLUETOOTH_OFF
        else -> reason
    }

    /**
     * Why [link] ended, settled: the one reason for the end of a link, for the head unit's page and for whatever else
     * ran over the link, such as a terminal ([com.nbtapps.store.data.terminal.JobTerminalLink]), so that the two cannot
     * differ. [reason] is the link's own. What the agent said of why it ended the link stands. A Bluetooth serial link
     * ended by the phone's Bluetooth going off says so ([BLUETOOTH_OFF]): its read fails as Bluetooth goes off, a
     * moment before or after the phone says it is off, so where Bluetooth still looks on this waits up to
     * [BLUETOOTH_OFF_WITHIN_MS] for it to go off. Every other end is answered at once.
     */
    suspend fun endReason(link: AgentSession, reason: String): String {
        val now = endReasonNow(link, reason)
        if (now != reason || link.closing != null || link.kind != Link.SERIAL) return now
        return withTimeoutOrNull(BLUETOOTH_OFF_WITHIN_MS) { bluetoothOn.first { !it } }?.let { BLUETOOTH_OFF } ?: reason
    }

    /**
     * The page's reason for the end of [link], shown at once as [failed], made the settled one ([endReason]) where that
     * differs, if nothing has been shown since. [ended] is the link's own reason.
     */
    private suspend fun settleEnd(link: AgentSession, ended: String, failed: ConnectionState.Failed) {
        val settled = endReason(link, ended)
        if (settled != failed.reason) connectionState.compareAndSet(failed, failed.copy(reason = settled))
    }

    /**
     * Makes [next], which the agent answers on, the app's link: reads what the head unit holds and follows its job.
     * Where [limited], as on a connect, each request is given [Greeting.answerMs] for its answer ([answered]).
     */
    private suspend fun finish(unit: HeadUnit, endpoint: Endpoint, next: AgentSession, hello: Hello, info: AgentInfo, limited: Boolean = false) {
        // The id is kept only now: before `auth` it was no more than what the other side said of itself.
        headUnits.update(unit.id) { it.copy(headUnitId = hello.headUnitId) }
        val installed = try {
            answered("packages.list", limited) { next.client.packages() }
        } catch (e: RemoteException) {
            if (e.code != "busy") throw e
            unit.profile?.installed.orEmpty()
        }
        session = next
        if (forgottenBy == unit.id) forgottenBy = null
        show(unit.id, endpoint, info, installed, answered("autostart.get", limited) { autostartOf(next) }, next.client.capabilities)
        val before = jobState.value
        follow(next, unit.id, limited)
        // A job of this app's that ended while there was no link, as the agent's own update does: it ends on the
        // new version, which the `job.status` just asked is what confirms. What was read above is from before
        // that, so it is read again, as it is after any job that ends with the link up.
        val after = jobState.value
        if (before != null && !before.ended && after != null && after.ended && after.planId == before.planId && after.headUnitId == unit.id) {
            refresh()
        }
        scope.launch { pump(next, unit.id, requireNotNull(hello.headUnitId)) }
        // Once for a head unit, and not waited for: see [ensureChecked].
        scope.launch { ensureChecked() }
    }

    /** Publishes what the agent reported, and keeps it with the head unit for when the phone is not in the car. */
    private suspend fun show(
        headUnitId: String,
        endpoint: Endpoint,
        info: AgentInfo,
        installed: List<InstalledPackage>,
        autostart: Autostart?,
        capabilities: List<String>,
    ) {
        connectionState.value = ConnectionState.Connected(headUnitId, endpoint, info, installed, autostart, capabilities)
        val serial = info.serial as? JsonObject
        serial?.let { serialStatus[headUnitId] = it }
        // Kept from every `info.get`, for how the next connect is made: see [HeadUnit.phoneOpenedLeft].
        val left = (serial?.get(PHONE_OPENED_LEFT) as? JsonPrimitive)?.booleanOrNull == true
        val profile = HeadUnitProfile(info.agentVersion, info.manifestVersion, info.free, info.restartNeeded, installed, now())
        headUnits.update(headUnitId) { it.copy(profile = profile, phoneOpenedLeft = left) }
    }

    /**
     * Picks up the job the agent has on record, if any, with the log lines this app has not seen. Where [limited],
     * each `job.status` is given [Greeting.answerMs] for its answer ([answered]).
     */
    private suspend fun follow(current: AgentSession, headUnitId: String, limited: Boolean) {
        val known = jobState.value?.takeIf { it.headUnitId == headUnitId }
        val found = answered("job.status", limited) { current.client.status() }.job
        if (found == null) {
            // A job of ours that had not ended is gone with the head unit's staging area.
            if (known != null && !known.ended) jobState.value = known.copy(forgotten = true, linkLost = false, reconnecting = false, waitedOut = false)
            return
        }
        val same = known?.takeIf { it.planId == found.planId }
        var view = (same ?: JobView(found.planId, headUnitId, plan = null, job = found))
            .copy(job = found, linkLost = false, forgotten = false, reconnecting = false, waitedOut = false)
        var after = same?.job?.seq ?: 0
        val replayed = ArrayList<JobLog>()
        while (true) {
            val status = answered("job.status", limited) { current.client.status(found.planId, after) }
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
            // before says nothing and closes its end, which does not end a serial link: one the phone goes on holding,
            // and writing to, leaves bytes in the car's Bluetooth ahead of every later link's. So the link is ended
            // here, for both.
            if (event is JobEvent.Progress && event.job.state == JobState.RESTARTING) current.closeAs(LinkClosing.RESTARTING)
            if (event is JobEvent.Done) {
                current.blobs.jobEnded()
                held = null
                refresh()
                // Checks that waited for the job, if any did.
                scope.launch { ensureChecked() }
            }
        }
        val ended = current.ended.await()
        val told = current.closing
        // What the agent said of why it ended the link stands; otherwise the phone's Bluetooth going off may be why.
        val reason = endReasonNow(current, ended)
        log.line("the link ended: $ended" + told?.let { " (link.closing $it)" }.orEmpty() + if (reason != ended) " ($reason)" else "")
        val failed = connectLock.withLock {
            // The app has moved on to another link: the end of this one, `other-link` too, is nothing to show.
            if (session !== current) return@withLock null
            session = null
            if (resetting === current) {
                // The car ended the link because the app asked it to, and the app connects again: see [resetSerial].
                connectionState.value = ConnectionState.Connecting(headUnitId)
                return@withLock null
            }
            val unfinished = { view: JobView? -> view != null && !view.ended && view.headUnitId == headUnitId }
            if (told == LinkClosing.RESTARTING) {
                // The job is not lost with the link: the app goes after it. The service that keeps the process
                // alive for a job goes on doing so meanwhile.
                // The wait is there to be ended before anything shows it: a disconnect that follows what is shown
                // finds it.
                rejoining.set(scope.launch { rejoin(headUnitId) })
                jobState.update { if (unfinished(it)) it?.copy(reconnecting = true, linkLost = false, waitedOut = false) else it }
                connectionState.value = ConnectionState.Connecting(headUnitId)
                return@withLock null
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
            jobState.update { if (unfinished(it)) it?.copy(linkLost = true, waitedOut = false) else it }
            ConnectionState.Failed(headUnitId, reason).also { connectionState.value = it }
        }
        if (failed != null) settleEnd(current, ended, failed)
    }

    private fun apply(headUnitId: String, event: JobEvent) = jobState.update { known ->
        val same = known?.takeIf { it.planId == event.planId && it.headUnitId == headUnitId }
        // Events come to every controller, so a job another one started shows up here by its first event.
        val view = same ?: JobView(event.planId, headUnitId, plan = null, job = JobSnapshot(event.planId, JobState.INSTALLING))
        // An event already seen: replayed by job.status, or sent while that was being answered.
        if (same != null && event.seq <= same.job.seq) return@update known
        when (event) {
            is JobEvent.Progress ->
                view.copy(job = event.job.copy(seq = event.seq), linkLost = false, forgotten = false, reconnecting = false, waitedOut = false)
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
        jobState.update { it?.takeIf { view -> !view.ended }?.copy(linkLost = true, waitedOut = false) ?: it }
    }

    /**
     * Runs [block] as a use of the link: while it runs, the app leaving the foreground does not end the link, and
     * once it is over the link goes if the app is out of sight then ([watchForeground]). For app-scoped code that
     * waits for the agent's answer, over one call or over several in a row; and for a connection over the link to a
     * program on the head unit, such as a terminal's shell, which waits for what the user types and is held for as
     * long as it is open ([usingSession]). Not for a wait for the user's answer otherwise. The repository's own calls
     * to the agent count as uses already.
     */
    suspend fun <T> usingLink(block: suspend () -> T): T = using { block() }

    /**
     * Runs [block] with the link to the head unit [headUnitId], as a use of the link ([usingLink]): for app-scoped code
     * that speaks over the link itself, as a terminal does over a channel to the SSH package
     * ([AgentSession.openChannel]). Fails where the link in use is not to that head unit, or is not up; a refusal of the
     * agent's and an [IOException] come out as with the repository's own calls. A link that ends meanwhile ends what
     * [block] has open over it.
     */
    suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T> = withSession { current ->
        if ((connectionState.value as? ConnectionState.Connected)?.headUnitId != headUnitId) throw IOException(NOT_CONNECTED)
        block(current)
    }

    /**
     * Whether the channel [channel] of the package [packageId] on the head unit [headUnitId] has a switch, whether it is
     * on, and how many connections of it are open on every link (`channel.get`), over the link to that head unit
     * ([usingSession]). A refusal is an [AgentRefusal]: `not-found`, `busy` while an install step runs, `unsupported`
     * from an agent without channels.
     */
    suspend fun channelSwitch(headUnitId: String, packageId: String, channel: String): Result<ChannelSwitch> =
        usingSession(headUnitId) { it.client.channelGet(packageId, channel) }

    /**
     * Switches the switched channel [channel] of the package [packageId] on the head unit [headUnitId] on or off
     * (`channel.set`), and gives how it then stands, as [channelSwitch] does. Off closes its open connections. A refusal is
     * an [AgentRefusal]: `busy` while a job runs or the agent is on trial, `bad-request` for a channel with no switch,
     * `not-found`, `forbidden`, `unsupported`, `io-error`.
     */
    suspend fun setChannelSwitch(headUnitId: String, packageId: String, channel: String, on: Boolean): Result<ChannelSwitch> =
        usingSession(headUnitId) { it.client.channelSet(packageId, channel, on) }

    /** [usingLink], for the repository's own calls: inline, so that a call returns from within it. */
    private inline fun <T> using(block: () -> T): T {
        inUse.update { it.copy(now = it.now + 1) }
        try {
            return block()
        } finally {
            inUse.update { Uses(it.now - 1, it.ended + 1) }
        }
    }

    private suspend fun <T> withSession(block: suspend (AgentSession) -> T): Result<T> = using {
        val current = session ?: return Result.failure(IOException(NOT_CONNECTED))
        return try {
            Result.success(block(current))
        } catch (e: RemoteException) {
            Result.failure(AgentRefusal(e.code, AgentText.request(e.code), e.error))
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    private companion object {
        const val MAX_LOG_LINES = 2_000
        const val NOT_CONNECTED = "Not connected to a head unit"
        const val NO_DETACHED_SIGNATURE = "This head unit's agent can't install community packages. Update the agent first."
        const val NO_FILES = "This head unit's agent can't send log files. Update the agent first."
        const val NO_CHECKS = "This head unit's agent has no checks. Update the agent first."
        const val PULL_BUSY = "Wait for the install to finish, then try again."

        /** Why the link is ended, and a connect given up, when the app leaves the foreground with nothing using the link. */
        const val LEFT = "The app was left, with nothing using the link"
        const val LEFT_WHILE_CONNECTING = "The app was left before the link was up"

        const val NO_SERIAL_RESET = "This head unit's agent can't reset the Bluetooth link. Update the agent first."
        const val RESET_WHILE_JOB = "Wait for the job to finish, then try again."

        /** A knock that brought no link from the car, where the car has no media audio connection to the phone: see [knockFailure]. */
        const val KNOCK_NO_MEDIA_AUDIO = "The car didn't connect to this app. It has no media audio connection to this phone: " +
            "choose this phone in the car's media menu to reconnect it, then connect again."

        /** A knock that brought no link from the car otherwise. */
        const val KNOCK_NO_LINK = "The car didn't connect to this app. Check that the agent is running and the car is in range."

        /** A knock that did not reach the car, which has no connection to the phone either: see [knockFailure]. */
        const val KNOCK_NOT_IN_REACH = "The car isn't in reach of this phone. Check that the car is on and nearby, then connect again."

        /** After the first two, the phone's own connect, the last thing to try. */
        const val KNOCK_FROM_PHONE = "If it still fails, try Connect from the phone, which can interrupt another app's link to the car."

        /** Why a link the phone opened is closed when a link the car opened takes its place: see [greet]. */
        const val CAR_LINK_TAKEN = "The car's link was taken in its place"

        /** The member of `info.get`'s `serial` that says the phone's own links are left: see [HeadUnit.phoneOpenedLeft]. */
        const val PHONE_OPENED_LEFT = "phoneOpenedLeft"

        /** Why the app ends a link the car was asked to end and did not. */
        const val RESET_BY_APP = "The car was asked to end the link, and the app ended it"

        /** Why a Bluetooth serial link ended where the phone's Bluetooth was switched off: see [endReason]. */
        const val BLUETOOTH_OFF = "This phone's Bluetooth was switched off"

        /**
         * How long after a Bluetooth serial link has ended the phone's Bluetooth going off is still taken for why it
         * ended: the phone says it is off a moment after the link's read has failed, or before.
         */
        const val BLUETOOTH_OFF_WITHIN_MS = 2_000L

        /** How long an `armed` reset lasts where the agent's answer does not say: the agent's own time. */
        const val ARMED_FOR_MS = 60_000L

        /** The time in a ZIP's name: the phone's own, as the user reads it. */
        val ZIP_TIME: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")
    }
}

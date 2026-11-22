// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.ConnectFailedException
import com.nbtapps.store.data.connectWithRetry
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.SerialEndpoint
import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

/** What the connector needs of the phone's Bluetooth, kept apart from it so that the rest runs in a unit test. */
interface SerialRadio {
    /**
     * Why no serial link to the device at [address] can be tried at the moment, in words for the user: no
     * permission, Bluetooth off, not paired. Null if one can.
     */
    fun unavailable(address: String): String?

    /** A socket to the service [uuid] on the paired device at [address], not yet connected. */
    fun socket(address: String, uuid: UUID): SerialSocket

    /**
     * Listens on this phone for serial links to the service [uuid], registered under [name], which a paired device
     * opens: the secure kind, as [socket]'s, so only a device paired with the phone gets through. See [SerialListener].
     *
     * @throws IOException if the phone cannot listen now: Bluetooth is off, the permission is not granted, or the
     *   phone refuses. A radio that cannot listen at all always throws.
     */
    fun listen(name: String, uuid: UUID): SerialServer = throw IOException("This phone can't listen for the car")

    /**
     * What the phone can tell of a try with [socket] to or from the device at [address], beyond its [failure], for
     * the diagnostics ([LinkTry]). A radio that can tell nothing says so, with every fact null.
     */
    fun inspect(address: String, socket: SerialSocket?, failure: Throwable?): TryFacts = TryFacts()
}

/** Listening for serial links to one service on the phone: see [SerialRadio.listen]. */
interface SerialServer : Closeable {
    /**
     * Blocks until a device opens a link to the service, and returns it, up.
     *
     * @throws IOException once [close] has been called, or when listening fails, as it does when Bluetooth is
     *   switched off.
     */
    fun accept(): AcceptedSocket

    /** Stops listening, and ends an [accept] under way. Safe to call twice, and from any thread. */
    override fun close()
}

/** A link a device opened to the phone: its socket, which is up, and the device's Bluetooth [address]. */
class AcceptedSocket(val socket: SerialSocket, val address: String)

/** One RFCOMM socket. */
interface SerialSocket : Closeable {
    /**
     * Blocks until the link is up.
     *
     * @throws IOException if it does not come up, or [close] is called meanwhile.
     */
    fun connect()

    val input: InputStream
    val output: OutputStream

    /** Ends the link, or a [connect] under way. Safe to call twice, and from any thread. */
    override fun close()
}

/**
 * How long a Bluetooth serial connect is tried for, and how: see [BluetoothSerialLinkConnector]. Times in
 * milliseconds; a test shortens them.
 */
class SerialConnectTimes(
    /** How long a connect is tried, to a head unit whose agent is not known to dial: longer than another app holds the car's link unanswered. */
    val windowMillis: Long = 20_000,
    /**
     * How long, to one whose agent is known to dial ([com.nbtapps.store.model.HeadUnit.serialDial]): long enough for
     * the car's own dial to come, which a refused dial of the phone's brings within seconds, and a poll within half a
     * minute. Also how long a connect that only listens waits for the car's link: a poll's time, and the ask and the
     * dial that follow it.
     */
    val dialWindowMillis: Long = 40_000,
    /** The most one try of the phone's is given: a connect that hears nothing blocks for about a minute. */
    val tryMillis: Long = 15_000,
    /** The pause after the first try that failed; each after it is twice the one before, up to [maxPauseMillis]. */
    val pauseMillis: Long = 700,
    val maxPauseMillis: Long = 5_000,
    /** A knock ([BluetoothSerialLinkConnector.knock]): the connects of one burst at most, as the protocol has it. */
    val knockAttempts: Int = ProtocolConstants.SERIAL_KNOCK_ATTEMPTS,
    /** From the end of one of a burst's connects to the start of the next, as the protocol has it. */
    val knockGapMillis: Long = ProtocolConstants.SERIAL_KNOCK_GAP_MS,
    /** How long a knock's connect that came up is kept for the car's link, as the protocol has it. */
    val knockHoldMillis: Long = ProtocolConstants.SERIAL_KNOCK_HOLD_MS,
    /**
     * The most one of a knock's connects is given to come up or fail. Most fail or come up well within it; one that has
     * no answer in it has not reached the car, and ends the knock ([BluetoothSerialLinkConnector.knock]).
     */
    val knockTryMillis: Long = 5_000,
    /** How many bursts a knock makes at most, where its caller does not say. */
    val knockBursts: Int = 3,
    /** After the last connect of a burst, how long the car's link is waited for before the burst counts as unanswered. */
    val knockSettleMillis: Long = 1_500,
    /** The pause between two bursts, after that wait, in which the car's link is still taken. */
    val knockPauseMillis: Long = 2_000,
)

/**
 * Bluetooth serial (RFCOMM) to the agent's own service on the car. The phone dials, so it speaks first; the
 * agent writes nothing until a valid `hello` has come, because every serial link a phone opens to the car lands
 * on the same node there, another app's included.
 *
 * What the phone says first is how the agent tells its links from those of other programs on the car: it waits
 * [ProtocolConstants.SERIAL_DECIDE_MS] from its open of a link for the magic bytes, passing over whole frames that
 * an earlier link left unread, and lets the link go without them. So they are written here, the moment the connect
 * comes back, and not with the first frame, which waits for whoever sends it. The link says so ([Link.magicSent]),
 * and its [Peer] sends none.
 *
 * Closing a link's node on the car does not end the link: the phone ends it, or the car does when the agent asks it
 * to ([AgentClient.serialReset]). The agent reads a link it is done with until then, so that nothing the phone still
 * writes is left in the car's Bluetooth for the next link.
 *
 * The car takes one serial link from a phone at a time. While another app on the phone holds it, a connect
 * fails at once, so a failed connect is tried again for a while ([SerialConnectTimes]). During a wireless Android
 * Auto session that link is held throughout. An agent that lists [ProtocolConstants.CAPABILITY_SERIAL_DIAL] gets
 * round that: the car opens a link of its own to the phone, which a refused dial of the phone's brings about within
 * seconds. So while the phone dials, it also listens for that link ([SerialListener], the app's one listener), and
 * the first link that is up, with the magic bytes written, is the one returned. The other is given up, or closed at
 * once if it came up too, before anything else is written on it.
 *
 * Where such an agent said that a link this phone opens itself is left to another program on the car
 * ([com.nbtapps.store.model.HeadUnit.phoneOpenedLeft]), a link of the phone's would come up and never be answered,
 * and would hold the car's one link for phone-opened links from that program. So a connect there only listens
 * (`listenOnly`): no dial of the phone's is made, and the car's link is waited for, which it opens when it next looks
 * for the app, every 10 to 30 seconds while the phone is connected to it. A wait that brings none fails with
 * [NoCarLinkException]; listening that cannot be had falls back to the phone's own dial, as above.
 *
 * An agent that lists [ProtocolConstants.CAPABILITY_SERIAL_KNOCK] has the car dial a phone only when that phone's app
 * knocks ([com.nbtapps.store.model.HeadUnit.serialKnock]): it cannot tell by itself whether the app listens. So a
 * connect to it knocks ([knock]): it listens for the car's link and makes bursts of short connects to the agent's
 * service. One that comes up carries the knock's magic bytes and nothing else, and is closed once the car's link has
 * come or its hold is over; one the car refuses reaches no agent and is no knock, so where the car refuses the phone's
 * links (it has no media audio connection to the phone) the app cannot knock. A burst has several connects because
 * one may fail on the phone while another app's link to the car is up. A connect that does not reach the car at all
 * (no answer in its time, or a failure while the phone has no connection to the car) ends the knock at once: the car
 * is off or out of range, and more connects would only keep the user waiting. The car's link that follows a knock is
 * the link, and no link of the phone's own is made.
 *
 * The socket is the authenticated, encrypted kind, so the link is to the car the phone is paired with, and
 * nobody listens in. That does not say who opened it: any app on a paired phone can. The agent therefore asks
 * for `auth` on this link, with a key it issued when the user typed a code from the car's screen into this app
 * (`JobRepository`, `PairingKeys`).
 */
class BluetoothSerialLinkConnector(
    private val radio: SerialRadio,
    private val times: SerialConnectTimes = SerialConnectTimes(),
    /** The clock the times are measured on: `SystemClock.elapsedRealtime` in the app, which goes on while the phone sleeps. */
    private val now: () -> Long = ::monotonicMillis,
    private val io: CoroutineDispatcher = Dispatchers.IO,
    /** Where each try is kept for the user to copy, the listener's too. */
    private val diagnostics: SerialDiagnostics = SerialDiagnostics(),
    private val log: LinkLog = LinkLog.NONE,
    /** The app's one listener, which [accept] waits on too. */
    private val listener: SerialListener = SerialListener(radio, io, diagnostics, log, now),
) : LinkConnector {

    override fun supports(endpoint: Endpoint): Boolean = endpoint is SerialEndpoint

    override suspend fun connect(endpoint: Endpoint): Link = connect(endpoint, dials = false)

    override suspend fun connect(endpoint: Endpoint, dials: Boolean): Link = connect(endpoint, dials, listenOnly = false)

    /**
     * Dials [endpoint] and listens for its car at once, for [SerialConnectTimes.dialWindowMillis] where the agent
     * [dials] and [SerialConnectTimes.windowMillis] where it is not known to. Listening that cannot be had leaves the
     * dial to go on alone. With [listenOnly], for an agent that [dials], only listens, for
     * [SerialConnectTimes.dialWindowMillis] ([waitForCar]). A caller that is cancelled leaves nothing open.
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    override suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean): Link {
        val serial = endpoint as? SerialEndpoint ?: throw IOException("Not a Bluetooth serial endpoint")
        radio.unavailable(serial.address)?.let { throw IOException(it) }
        if (listenOnly && dials) waitForCar(serial)?.let { return it }
        val window = if (dials) times.dialWindowMillis else times.windowMillis
        // The first link that is up. A second one finds it taken, and is closed with nothing more written on it.
        val won = CompletableDeferred<Link>()
        fun offer(link: Link, path: String) {
            if (won.complete(link)) {
                log.line("connect: the $path link is up and taken")
            } else {
                link.close()
                log.line("connect: the $path link came up second and was closed")
            }
        }
        // This connect's own tries, for why none came up.
        val tries = CopyOnWriteArrayList<LinkTry>()
        try {
            return coroutineScope {
                val listening = launch {
                    try {
                        offer(listener.accept(listOf(serial.address), first = true).link, "car's")
                    } catch (e: IOException) {
                        // The phone cannot listen now: the dial goes on alone.
                    }
                }
                val dialling = launch {
                    try {
                        offer(SerialLink(connectWithRetry(window, times.pauseMillis, now, times.maxPauseMillis) { dial(serial.address, tries) }), "phone's")
                    } catch (e: ConnectFailedException) {
                        log.line("connect: gave up after ${e.tries}")
                        won.completeExceptionally(IOException(gaveUp(serial, e, causeOf(tries), dials), e))
                    }
                }
                try {
                    won.await()
                } finally {
                    listening.cancel()
                    dialling.cancel()
                }
            }
        } catch (e: CancellationException) {
            // The caller gave up: while the connect waited, or once a link had won, while the other way was still being
            // ended. A link that won is nobody's then, and must not hold the car's link.
            closeWon(won, e)
            throw e
        }
    }

    /** A link [won] holds, which a caller cancelled with [cause] will never have, is closed; one still to come is too. */
    private fun closeWon(won: CompletableDeferred<Link>, cause: CancellationException) {
        if (!won.completeExceptionally(cause) && won.getCompletionExceptionOrNull() == null) won.getCompleted().close()
    }

    /**
     * Waits for the car at [serial] to open a link to the phone, for [SerialConnectTimes.dialWindowMillis], and makes
     * no link of the phone's: see the class. The wait is kept in the diagnostics either way. Null where the phone cannot
     * listen, for the caller to dial as well.
     *
     * @throws NoCarLinkException if no link came in that time.
     */
    private suspend fun waitForCar(serial: SerialEndpoint): Link? {
        val began = now()
        val accepted = try {
            withTimeoutOrNull(times.dialWindowMillis) { listener.accept(listOf(serial.address), first = true) }
        } catch (e: IOException) {
            log.line(diagnostics.record(LinkTry.Path.LISTEN_ONLY, LinkTry.Outcome.FAILED, now() - began, e).describe())
            log.line("connect: the phone can't listen for the car, so it dials it as well")
            return null
        }
        if (accepted == null) {
            log.line(diagnostics.record(LinkTry.Path.LISTEN_ONLY, LinkTry.Outcome.NO_LINK, now() - began).describe())
            throw NoCarLinkException(noCarLink())
        }
        log.line(diagnostics.record(LinkTry.Path.LISTEN_ONLY, LinkTry.Outcome.TAKEN, now() - began).describe())
        return accepted.link
    }

    /** What the user is told when a connect that only listened had no link from the car. */
    private fun noCarLink(): String =
        "The car didn't connect to this app within ${times.dialWindowMillis / 1000} s. The app waits for the car here, " +
            "which looks for it every 10 to 30 seconds while the phone is connected over Bluetooth. Check that the agent " +
            "is running and the car is in range."

    /**
     * Knocks at the agent of [endpoint] and takes the link its car then opens: see the class. The phone listens for the
     * car's link through the app's one listener, ahead of anyone else waiting there, from the start to the end, and
     * meanwhile makes up to [bursts] bursts, or [SerialConnectTimes.knockBursts]. A burst is up to
     * [SerialConnectTimes.knockAttempts] connects to the agent's service, each [SerialConnectTimes.knockGapMillis] after
     * the end of the one before, whatever came of it ([knockOnce]); after its last, the car's link is waited for
     * [SerialConnectTimes.knockSettleMillis], and the next burst follows [SerialConnectTimes.knockPauseMillis] later.
     * The car's link ends the knocking the moment it is handed over, with the link's magic bytes written on it, and is
     * returned. Each connect, each burst that had nothing and what the knocking came to are kept in the diagnostics.
     *
     * A connect that does not reach the car ([knockOnce]) ends the knocking at once, with no connect after it: the car
     * is off or out of range then, and the rest would only take their time, with the user waiting. The first connect is
     * always made, so a car in range whose connection to the phone that connect brings up is knocked at all the same.
     *
     * A caller that is cancelled leaves nothing open: no connect of the knock's, no wait on the listener, no car's link,
     * even one handed over as the caller gave up.
     *
     * @throws KnockUnansweredException if no link of the car's came after the last burst, or, [KnockUnansweredException.carAway],
     *   after a connect that did not reach the car.
     * @throws NoCarLinkException if the phone cannot listen for it: then knocking would bring a link nobody takes.
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    override suspend fun knock(endpoint: Endpoint, bursts: Int?): Link {
        val serial = endpoint as? SerialEndpoint ?: throw IOException("Not a Bluetooth serial endpoint")
        radio.unavailable(serial.address)?.let { throw IOException(it) }
        val most = (bursts ?: times.knockBursts).coerceAtLeast(1)
        val began = now()
        // The car's link, once it has been handed over; or why there is none.
        val came = CompletableDeferred<Link>()
        // Where the knocking is, for what the diagnostics say brought the link.
        val at = AtomicReference<String?>(null)
        fun record(outcome: LinkTry.Outcome, failure: Throwable? = null, facts: TryFacts = TryFacts(), detail: String? = null) =
            log.line(diagnostics.record(LinkTry.Path.KNOCK_CONNECT, outcome, now() - began, failure, facts, detail).describe())
        val link = try {
            coroutineScope {
                val listening = launch {
                    try {
                        val accepted = listener.accept(listOf(serial.address), first = true)
                        if (!came.complete(accepted.link)) accepted.link.close()
                    } catch (e: IOException) {
                        came.completeExceptionally(NoCarLinkException("This phone can't listen for the car's link: ${e.message}", e))
                    }
                }
                val knocking = launch {
                    for (burst in 1..most) {
                        if (burst > 1 && carCame(came, times.knockPauseMillis)) return@launch
                        val burstBegan = now()
                        for (attempt in 1..times.knockAttempts) {
                            if (attempt > 1 && carCame(came, times.knockGapMillis)) return@launch
                            at.set("in burst $burst of $most, after connect $attempt")
                            val reached = knockOnce(serial.address, came, "$burst.$attempt")
                            if (came.isCompleted) return@launch
                            if (!reached) {
                                came.completeExceptionally(KnockUnansweredException(KNOCK_UNANSWERED, carAway = true))
                                return@launch
                            }
                        }
                        if (carCame(came, times.knockSettleMillis)) return@launch
                        log.line(
                            diagnostics.record(LinkTry.Path.KNOCK_BURST, LinkTry.Outcome.NO_LINK, now() - burstBegan, detail = "$burst of $most").describe(),
                        )
                    }
                    came.completeExceptionally(KnockUnansweredException(KNOCK_UNANSWERED))
                }
                try {
                    came.await()
                } catch (e: KnockUnansweredException) {
                    val detail = when {
                        e.carAway -> "${at.get()}, which did not reach the car,"
                        most == 1 -> "of 1 burst"
                        else -> "of $most bursts"
                    }
                    record(LinkTry.Outcome.NO_LINK, facts = facts(serial.address, null, null), detail = detail)
                    throw e
                } catch (e: NoCarLinkException) {
                    record(LinkTry.Outcome.FAILED, e.cause)
                    throw e
                } finally {
                    listening.cancel()
                    knocking.cancel()
                }
            }
        } catch (e: CancellationException) {
            // The caller gave up: while the knock waited, or once the car's link had been handed over, while the knock's
            // own connects were still being ended. A link handed over is nobody's then, and must not hold the car's link.
            closeWon(came, e)
            record(LinkTry.Outcome.CANCELLED)
            throw e
        }
        record(LinkTry.Outcome.TAKEN, detail = at.get())
        return link
    }

    /** Waits up to [millis] for [came] to be settled, a link or a failure, and says whether it is. */
    private suspend fun carCame(came: CompletableDeferred<Link>, millis: Long): Boolean {
        withTimeoutOrNull(millis) { came.join() }
        return came.isCompleted
    }

    /**
     * One of a knock's connects, [which] by its burst and its place in it, given [SerialConnectTimes.knockTryMillis].
     * One that comes up is sent the knock's magic bytes and nothing else, nothing is read from it, and it is closed once
     * [came] is settled or [SerialConnectTimes.knockHoldMillis] have passed. One that fails, is refused, or hears
     * nothing is closed at once. A cancel, as when the car's link has come, closes it wherever it is. What came of it is
     * kept in the diagnostics, in one line: a connect that is cancelled after its line, as one that came up is in its
     * hold, has no second.
     *
     * Whether it reached the car: false where it had no answer in its time, or failed while the phone had no connection
     * to the car ([TryFacts.carConnected] false); then the car is off or out of range, and the knock ends ([knock]). One
     * that came up, was refused by the car, or failed while the phone was connected to the car, or could not tell, did.
     * Any other failure is no failure of the knock, which goes on.
     */
    private suspend fun knockOnce(address: String, came: CompletableDeferred<Link>, which: String): Boolean {
        val began = now()
        var made: SerialSocket? = null
        var noted = false
        fun note(outcome: LinkTry.Outcome, failure: Throwable?, facts: TryFacts) {
            noted = true
            log.line(diagnostics.record(LinkTry.Path.KNOCK, outcome, now() - began, failure, facts, which).describe())
        }
        try {
            val socket = withContext(io) { radio.socket(address, SERVICE_UUID).also { made = it } }
            withTimeoutOrNull(times.knockTryMillis) { withContext(io) { bringUp(socket, FrameCodec.KNOCK_MAGIC) } } ?: run {
                socket.closeQuietly()
                throw NoAnswer(times.knockTryMillis)
            }
            note(LinkTry.Outcome.KNOCKED, null, facts(address, socket, null))
            try {
                carCame(came, times.knockHoldMillis)
            } finally {
                socket.closeQuietly()
            }
            return true
        } catch (e: IOException) {
            val facts = facts(address, made, e)
            // The socket found the agent's channel, so the request reached the car, which turned it down.
            val outcome = when {
                e is NoAnswer -> LinkTry.Outcome.NO_ANSWER
                facts.channel != null && facts.channel >= 0 -> LinkTry.Outcome.REFUSED
                else -> LinkTry.Outcome.FAILED
            }
            note(outcome, e, facts)
            return outcome == LinkTry.Outcome.REFUSED || (outcome == LinkTry.Outcome.FAILED && facts.carConnected != false)
        } catch (e: CancellationException) {
            made?.closeQuietly()
            if (!noted) note(LinkTry.Outcome.CANCELLED, null, TryFacts())
            throw e
        }
    }

    /** What the phone can tell of a try, which may not fail it: nothing, where asking fails. */
    private fun facts(address: String, socket: SerialSocket?, failure: Throwable?): TryFacts = try {
        radio.inspect(address, socket, failure)
    } catch (e: RuntimeException) {
        TryFacts()
    }

    override fun accepts(endpoint: Endpoint): Boolean = endpoint is SerialEndpoint

    /** Through the app's one listener, which a [connect] under way shares, and is given a link first on. */
    override suspend fun accept(endpoints: Collection<Endpoint>): IncomingLink {
        val serial = endpoints.filterIsInstance<SerialEndpoint>()
        if (serial.isEmpty()) return super.accept(endpoints)
        val accepted = listener.accept(serial.map { it.address })
        return IncomingLink(serial.first { it.address.equals(accepted.address, ignoreCase = true) }, accepted.link)
    }

    /**
     * What the user is told when no try came up: the [cause] the tries tell, where they do. Otherwise the phone
     * learns only that the connect failed, not why, so the text names what it can be: for an agent that [dials],
     * not another app holding the car's link, which such an agent gets round. Android's own words for the last
     * failure come last, for whoever can read them.
     */
    private fun gaveUp(serial: SerialEndpoint, failure: ConnectFailedException, cause: LinkCause?, dials: Boolean): String {
        val why = cause?.text(dials)
            ?: if (dials) {
                "The agent may not be running on the head unit, or the car may be out of range."
            } else {
                "The agent may not be running on the head unit, the car may be out of range, or a wireless Android Auto " +
                    "session may be holding the car's one serial link to this phone."
            }
        return "No Bluetooth serial link to ${serial.deviceName} came up (${failure.tries}). $why" +
            failure.cause?.message?.let { " Last failure: $it" }.orEmpty()
    }

    /** The phone's dial heard nothing within its time. */
    private class NoAnswer(millis: Long) : IOException("The car didn't answer within ${millis / 1000} s")

    /**
     * One try, given [SerialConnectTimes.tryMillis], and kept in [tries] and the diagnostics. A connect blocks for
     * seconds and hears of nothing but its socket closing, so a try that runs out of time, and one whose caller is
     * cancelled, closes it.
     */
    private suspend fun dial(address: String, tries: MutableList<LinkTry>): SerialSocket {
        val began = now()
        var made: SerialSocket? = null
        try {
            val socket = withContext(io) { radio.socket(address, SERVICE_UUID).also { made = it } }
            withTimeoutOrNull(times.tryMillis) { withContext(io) { bringUp(socket, FrameCodec.MAGIC) } } ?: run {
                socket.closeQuietly()
                throw NoAnswer(times.tryMillis)
            }
            tries += note(address, socket, LinkTry.Outcome.UP, null, began)
            return socket
        } catch (e: IOException) {
            tries += note(address, made, if (e is NoAnswer) LinkTry.Outcome.NO_ANSWER else LinkTry.Outcome.FAILED, e, began)
            throw e
        } catch (e: CancellationException) {
            // Cancelled, perhaps at the very moment the link came up: nobody is handed the socket, so it must not
            // stay open and hold the car's one serial link.
            made?.closeQuietly()
            throw e
        }
    }

    /** Keeps a try of the phone's dial, with what the phone can tell of it, and logs it. */
    private fun note(address: String, socket: SerialSocket?, outcome: LinkTry.Outcome, failure: IOException?, began: Long): LinkTry {
        val facts = try {
            radio.inspect(address, socket, failure)
        } catch (e: RuntimeException) {
            TryFacts()
        }
        return diagnostics.record(LinkTry.Path.DIAL, outcome, now() - began, failure, facts).also { log.line(it.describe()) }
    }

    /** Connects [socket] and writes [magic] on it: the link's magic bytes, or the knock's. */
    private suspend fun bringUp(socket: SerialSocket, magic: ByteArray) {
        // Set once the connect has come back, either way: from then on the socket is not the watcher's to close.
        val settled = AtomicBoolean(false)
        try {
            coroutineScope {
                // Started here and now, not handed to the dispatcher: a cancel that came before it had begun to
                // run would skip it, and the connect would sit out its time.
                val onCancel = launch(start = CoroutineStart.UNDISPATCHED) {
                    try {
                        awaitCancellation()
                    } finally {
                        if (!settled.get()) socket.closeQuietly()
                    }
                }
                try {
                    socket.connect()
                    // Part of bringing the link up: one that cannot take them is a try that failed.
                    socket.output.write(magic)
                    socket.output.flush()
                } finally {
                    settled.set(true)
                }
                onCancel.cancel()
            }
        } catch (e: IOException) {
            socket.closeQuietly()
            // A connect that failed because its caller was cancelled is a cancellation, not a try that failed.
            currentCoroutineContext().ensureActive()
            throw e
        }
    }

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString(ProtocolConstants.SERIAL_SERVICE_UUID)

        /** What [knock] fails with where no link of the car's came: what happened, which the caller words for the user. */
        const val KNOCK_UNANSWERED = "The car didn't connect to this app after it knocked"
    }
}

/** A serial link over [socket], whose magic bytes have gone out: one the phone dialled, or one it accepted, [openedByCar]. */
internal class SerialLink(private val socket: SerialSocket, override val openedByCar: Boolean = false) : Link {
    override val kind = Link.SERIAL
    override val input: InputStream = socket.input
    override val output: OutputStream = socket.output
    override val magicSent = true

    /** Closing the socket also fails a read that is blocked on it. */
    override fun close() = socket.closeQuietly()
}

internal fun Closeable.closeQuietly() {
    try {
        close()
    } catch (e: IOException) {
        // It is gone either way.
    }
}

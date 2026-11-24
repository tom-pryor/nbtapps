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
     * no answer in it, while the phone has no connection to the car that it can tell of, has not reached the car, and
     * ends the knock ([BluetoothSerialLinkConnector.knock]).
     */
    val knockTryMillis: Long = 5_000,
    /** How many bursts a knock makes at most, where its caller does not say. */
    val knockBursts: Int = 3,
    /** After the last connect of a burst, how long the car's link is waited for before the burst counts as unanswered. */
    val knockSettleMillis: Long = 1_500,
    /** The pause between two bursts, after that wait, in which the car's link is still taken. */
    val knockPauseMillis: Long = 2_000,
    /**
     * The pause before the next burst, after the wait for the car's link, where a burst ended because one of its connects
     * may have run into another app's link to the car ([BluetoothSerialLinkConnector.knock]): Android has closed that
     * app's link, which the app opens again at once, and a connect while it does so would fail with it and harm both.
     */
    val knockCollisionPauseMillis: Long = 6_000,
    /**
     * The wait, in place of [knockGapMillis], after a knock's connect to the agent's service that failed where the phone
     * cannot read whether it had found the service: it may have run into another app's link, whose next link is then up
     * again, not half made, when the knock's next connect goes out.
     */
    val knockUnsureWaitMillis: Long = 1_500,
    /**
     * A knock by dial-up ([BluetoothSerialLinkConnector.knock]): how long the car's link is waited for after the
     * connect to the car's dial-up networking service, from its end, before the burst goes on, as the protocol has it.
     */
    val knockDialupWaitMillis: Long = ProtocolConstants.SERIAL_KNOCK_DIALUP_WAIT_MS,
    /**
     * The most a connect to the car's dial-up networking service is given to come up or fail: shorter than
     * [knockTryMillis], as the agent refuses a request for it that the car has not refused within about 2 s. One that
     * has no answer in it is closed.
     */
    val knockDialupTryMillis: Long = 3_000,
    /**
     * The pause before the next burst, in place of [knockPauseMillis], after a burst that ended with its knock by dial-up
     * and no connect to the serial channel ([BluetoothSerialLinkConnector.knock]). Where that knock brought a dial of the
     * car's that failed, as one the phone is too busy to take, the agent ignores the phone's knocks for its quiet time,
     * 3 s from that dial's answer, and a knock inside it is lost. The dial goes out as the request ends, about when the
     * connect does, and such a dial is answered within a second or so; the next dial-up connect comes the wait for the
     * car's link ([knockDialupWaitMillis], 1.5 s) and this pause after the connect's end, 5.5 s, so outside it by about
     * a second and a half, where the usual pause would put it at 3.5 s, inside it.
     */
    val knockDialupPauseMillis: Long = 4_000,
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
 * links (it has no media audio connection to the phone) the app cannot knock. A burst has several connects, as one may
 * fail for a while. But every serial service of the car's is on one RFCOMM channel, and Android takes no second link
 * from the phone to the same channel of a device: a connect while another app on the phone has its link to the car
 * fails on the phone, and Android's clean-up of it closes that app's link, which the app then opens again at once. A
 * connect right after would fail with that half-made link and harm both, so a connect that may have run into another
 * app's link ends its burst, and the next burst waits longer ([knock]). A connect that does not reach the car at all
 * (no answer in its time, or a failure while the phone has no connection to the car) ends the knock at once: the car
 * is off or out of range, and more connects would only keep the user waiting. The car's link that follows a knock is
 * the link, and no link of the phone's own is made.
 *
 * Such an agent has the car dial any phone that knocks and is connected to the car and paired with it, one of its own
 * or not. So a connect to an agent not known to knock, as one never greeted yet, may knock first: a probing knock, one
 * burst that ends after the first of its connects that came up, while the connect already listens for the car's link
 * ([connect] with `probe`). Where it brings none, the phone dials as it would have. It costs an agent that does not
 * know knocks the knock's hold and the wait after it, some 3 seconds, and where no agent runs, one knock's link that
 * another program at the car may take.
 *
 * The car advertises only some of the serial services registered there: one registered after those it has room for
 * is in no service record, though the car still dials from it, so the phone finds no channel for it, and a connect to
 * it fails before it reaches the car. Every serial service of the car's is on the same RFCOMM channel, and the agent
 * tells a knock by its magic bytes, not by the service it was opened to. So where a knock's connect to the agent's
 * service finds no record of it, its next connect goes through the car's standard Serial Port service
 * ([STANDARD_SERVICE_UUID]) instead, with the same bytes and the same hold: one at most, in the whole knock, in each of
 * its bursts for a connect the user starts, or none for the wait after an agent's update ([KnockFallback], [knock]).
 * Where no agent runs, such a connect that comes up reaches another program at the car, which may count it against
 * its own service. A knock whose connects to the agent's service come up makes none. Only the knock does this; the
 * phone's own link is always to the agent's service.
 *
 * There are two kinds of knock, and a burst makes them in this order. To an agent that lists
 * [ProtocolConstants.CAPABILITY_SERIAL_KNOCK_DIALUP], and in a probing knock at one never greeted, each burst first
 * makes one connect to the car's dial-up networking service ([DIALUP_UUID]), the knock by dial-up: that service is on an
 * RFCOMM channel of its own, so the connect cannot run into another app's link to the car's serial channel. The car's
 * side hears the request, which is the knock, and the car or the agent refuses it, so the connect fails with nothing
 * written or read; the car's link is then waited for ([SerialConnectTimes.knockDialupWaitMillis]). An agent that hears
 * the one knock hears the other, and both bring the same dial, so once the knock by dial-up has been made the connects
 * to the serial channel add nothing and may only run into another app's link: where none came, the burst ends there and
 * the next begins with the knock by dial-up again, but for the knock's last burst, which goes on with its connects to
 * the serial channel, the knock by the magic bytes, as above, so that an agent that does not answer the knock by
 * dial-up is still reached, once.
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
    override suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean): Link = connect(endpoint, dials, listenOnly, probe = null)

    /**
     * As [connect] without [probe], but that with [probe] the dial follows a probing knock ([probeKnock]), for an agent that
     * is not known to knock: the phone listens for the car's link from the start, so a link the car opens at any time,
     * while the probe knocks, between its end and the dial's start, or during the dial, is taken. The probe takes nothing
     * from the dial: its window begins after the probe, and the probe's connects are not among the tries whose failures
     * the user is told of. A probe that ended because one of its connects may have run into another app's link to the
     * car has the dial wait [SerialConnectTimes.knockCollisionPauseMillis] first, as a knock's next burst does, while
     * the phone listens on. A connect that only listens makes none, as its agent leaves the phone's own links to another
     * program at the car, which a knock's link would reach. [dialup] is the probe's, as [knock]'s.
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    override suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean, probe: KnockFallback?, dialup: Boolean): Link {
        val serial = endpoint as? SerialEndpoint ?: throw IOException("Not a Bluetooth serial endpoint")
        radio.unavailable(serial.address)?.let { throw IOException(it) }
        if (listenOnly && dials) waitForCar(serial)?.let { return it }
        val probing = probe.takeIf { !listenOnly }
        // Set where the phone cannot listen: a probe then knocks no more, as the car's link would come to nobody.
        val deaf = AtomicBoolean(false)
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
                        deaf.set(true)
                    }
                }
                val dialling = launch {
                    if (probing != null) {
                        when (probeKnock(serial, won, probing, deaf, dialup)) {
                            ProbeEnd.TAKEN -> return@launch
                            ProbeEnd.IN_THE_WAY -> if (carCame(won, times.knockCollisionPauseMillis)) return@launch
                            ProbeEnd.NOTHING -> Unit
                        }
                    }
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
     * A connect that may have run into another app's link to the car ends its burst, with no connect after it in that
     * burst: Android has closed that app's link, the app opens it again at once, and a connect right after would fail
     * with that half-made link and harm both. That is a connect that failed (not one with no answer, and not one that
     * did not reach the car) where the service was there to be connected to: one to the agent's service where the phone
     * had found its channel ([Knocked.FAILED_FOUND]), and any one to the car's Serial Port service, which the car always
     * has. The car's link is still waited for [SerialConnectTimes.knockSettleMillis], and the next burst follows
     * [SerialConnectTimes.knockCollisionPauseMillis] later, in place of [SerialConnectTimes.knockPauseMillis]; where it
     * was the last burst, or the knock is [KnockFallback.ONCE_PER_KNOCK]'s (the app's own), the knock ends there, as
     * [KnockUnansweredException.inTheWay]. Such a connect may also be one the car refused: the phone cannot tell the two.
     *
     * The connects go to the agent's service. One that failed where the phone found no channel for it
     * ([Knocked.NOT_FOUND]: the car has no record of it a phone can find, and the connect went no further than the
     * look-up, disturbing nobody) has the next connect go to the car's standard Serial Port service
     * ([STANDARD_SERVICE_UUID]) at the gap. Where the phone cannot read whether it had found the channel
     * ([Knocked.UNSURE]), it cannot tell a missing record from a connect that ran into another app's link, so the next
     * connect, to the Serial Port service, waits [SerialConnectTimes.knockUnsureWaitMillis] in place of the gap, for that
     * app's link to be up again rather than half made. So the channel the phone reads, which Android does not publish,
     * only makes the knock quicker (no record) or more careful (another app's link in the way); where it cannot be read
     * the knock still works, a little slower. One connect to the Serial Port service at most is made, whatever came of it:
     * in the whole knock with [KnockFallback.ONCE_PER_KNOCK], the app's own knocks; in each burst with
     * [KnockFallback.ONCE_PER_BURST], a connect the user starts; none with [KnockFallback.NONE], the wait after an agent's
     * update, whose agent on trial dials the phone by itself. Where one of them came up and brought no link, the
     * burst's remaining connects go to the agent's service, whose last failure found no record, or could not tell. A
     * later burst begins at the Serial Port service where the agent's service last failed so, as long as it may make a
     * connect there, and at the agent's service where that last ran into another app's link, as the service is there.
     * Where nothing answers, a knock makes one connect to that service in all, or one in each burst; where no agent runs,
     * one that comes up reaches another program at the car. The connects to that service take the places of connects to
     * the agent's, and a knock makes no more connects than it would without them. The rule of the car away is the same
     * for both services. Each connect's line in the diagnostics names the Serial Port service where it went there, and
     * so does the knock's last line where the knocking was at such a connect; a burst that ended so says so in its
     * line, and with how long the next waits.
     *
     * A caller that is cancelled leaves nothing open: no connect of the knock's, no wait on the listener, no car's link,
     * even one handed over as the caller gave up.
     *
     * With [dialup], each burst first makes one connect to the car's dial-up networking service, the knock by dial-up
     * ([DIALUP_UUID]), before its connects of above, which it does not count among [SerialConnectTimes.knockAttempts],
     * in a knock of every kind ([dialupOnce]). Nothing is written on it. It fails where the car's side heard it:
     * - where the phone had found the service's channel, the knock was made; the car's link is waited for
     *   [SerialConnectTimes.knockDialupWaitMillis] from the connect's end, and where it comes no connect to the serial
     *   channel is made at all;
     * - where the phone cannot tell, the car's link is waited for as where the knock was made: so the channel the phone
     *   reads only makes the knock quicker, here as elsewhere;
     * - where the phone found no record of the service, the car does not have it on now: the burst goes on at once with
     *   its connects of above, in every burst.
     * One that has no answer in [SerialConnectTimes.knockDialupTryMillis] is closed; the request may still be pending at
     * the car, whose refusal brings the knock. Where the knock by dial-up was made, may have been, or had no answer, and
     * no link came, the burst ends there, with no connect to the serial channel: once that knock is made the others add
     * nothing and may only run into another app's link. After one that had no answer, the car's link is waited for
     * [SerialConnectTimes.knockSettleMillis] first, as at the end of any burst, since a refusal of the agent's at the car
     * brings its dial a moment after the phone has closed the connect; after the others, the wait after the connect is
     * that wait. The next burst then follows [SerialConnectTimes.knockDialupPauseMillis] later, outside the time in which
     * the agent ignores the phone's knocks after a dial of its that failed, and begins with the knock by dial-up again.
     * The knock's last burst (so the only burst of a knock of one, and a probing knock's) goes on with its connects of
     * above instead, at once after one with no answer, so that an agent that does not answer the knock by dial-up is
     * still reached, once. Such a burst is no collision: neither the collision pause nor [KnockUnansweredException.inTheWay]
     * comes of it. One that comes up, which nobody should let it, is closed at once with nothing written, the burst goes
     * on at once with its connects of above, and no other is made in that knock. One that does not reach the car ends
     * the knock, as any connect does. Its failure is never taken as one that ran into another app's link, nor turns the
     * next connect to the Serial Port service. Each such connect has its line in the diagnostics, numbered 0 in its
     * burst ("1.0"), so that the other connects keep their numbers; a burst that ended with it says so in its line, and
     * the knock's last line says where the car's link came after one.
     *
     * @throws KnockUnansweredException if no link of the car's came after the last burst, or, [KnockUnansweredException.carAway],
     *   after a connect that did not reach the car, or, [KnockUnansweredException.inTheWay], after a connect that may
     *   have run into another app's link ended the knock.
     * @throws NoCarLinkException if the phone cannot listen for it: then knocking would bring a link nobody takes.
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    override suspend fun knock(endpoint: Endpoint, bursts: Int?, fallback: KnockFallback, dialup: Boolean): Link {
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
                    when (knockAt(serial.address, came, most, fallback, at, dialup = dialup)) {
                        KnockEnd.AWAY -> came.completeExceptionally(KnockUnansweredException(KNOCK_UNANSWERED, carAway = true))
                        KnockEnd.UNANSWERED -> came.completeExceptionally(KnockUnansweredException(KNOCK_UNANSWERED))
                        KnockEnd.IN_THE_WAY -> came.completeExceptionally(KnockUnansweredException(KNOCK_UNANSWERED, inTheWay = true))
                        // Settled already, by the car's link or by the listener's failure.
                        KnockEnd.CAME, KnockEnd.CAME_UP, KnockEnd.DEAF -> Unit
                    }
                }
                try {
                    came.await()
                } catch (e: KnockUnansweredException) {
                    val detail = when {
                        e.carAway -> "${at.get()}, which did not reach the car,"
                        e.inTheWay -> "${at.get()}, $IN_THE_WAY_OF,"
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

    /** Where [knockAt] stopped. */
    private enum class KnockEnd {
        /** [knockAt]'s `came` was settled: by the car's link, or by why there is none. */
        CAME,

        /** A connect did not reach the car. */
        AWAY,

        /** After the last burst, with no link of the car's. */
        UNANSWERED,

        /**
         * After a connect that may have run into another app's link to the car, which ended the knock's last burst, or,
         * for [KnockFallback.ONCE_PER_KNOCK], the knock, with no link of the car's.
         */
        IN_THE_WAY,

        /** After its first connect that came up, its hold and the wait after it, with no link of the car's: a probe's end. */
        CAME_UP,

        /** Before a connect, as the phone cannot listen for the car's link. */
        DEAF,
    }

    /**
     * The knocking of [knock], [most] bursts at the car at [address], until [came] is settled, with its connects to the
     * car's Serial Port service as [fallback] allows, and [at] kept at the connect it is at; each burst that had nothing
     * and is followed by another is kept in the diagnostics. With [probing], it ends after the first of its connects that
     * came up, to either service, its hold and [SerialConnectTimes.knockSettleMillis] after it: an agent that knows
     * knocks has had the car dial by then. Before each connect it ends where [deaf] says that the phone cannot listen. It
     * settles nothing: what it stopped at is its caller's to make something of. With [dialup], each burst first makes
     * its connect to the car's dial-up networking service ([dialupOnce]) and waits for the car's link after it, and a
     * burst but the last ends there where that knock was made, may have been or had no answer, as [knock] says, until
     * one has come up; [at] then names that connect.
     */
    private suspend fun knockAt(
        address: String,
        came: CompletableDeferred<Link>,
        most: Int,
        fallback: KnockFallback,
        at: AtomicReference<String?>,
        probing: Boolean = false,
        deaf: () -> Boolean = { false },
        dialup: Boolean = false,
    ): KnockEnd {
        // Whether the knock may still make its connect to the dial-up service at the start of a burst.
        var dialupLeft = dialup
        // Whether the agent's service last failed where no record of it was found, or the phone could not tell: then
        // a burst begins at the Serial Port service, where it may make a connect there. See [knock].
        var missing = false
        // Whether the knock may still make its connect to the Serial Port service.
        var standardLeft = fallback != KnockFallback.NONE
        // Whether the last burst ended after a connect that may have run into another app's link.
        var inTheWay = false
        // The pause before the next burst, by how the last one ended.
        var pause = times.knockPauseMillis
        for (burst in 1..most) {
            if (burst > 1 && carCame(came, pause)) return KnockEnd.CAME
            if (fallback == KnockFallback.ONCE_PER_BURST) standardLeft = true
            inTheWay = false
            val burstBegan = now()
            if (dialupLeft) {
                if (deaf()) return KnockEnd.DEAF
                at.set("after the dial-up connect of burst $burst")
                val dialed = dialupOnce(address, "$burst.0 $TO_DIALUP")
                if (came.isCompleted) return KnockEnd.CAME
                when (dialed) {
                    // The knock was made, or may have been: the car's link is waited for.
                    Dialed.KNOCKED, Dialed.UNSURE -> if (carCame(came, times.knockDialupWaitMillis)) return KnockEnd.CAME
                    Dialed.AWAY -> return KnockEnd.AWAY
                    // Let up, which it should not be: none again in this knock.
                    Dialed.UP -> dialupLeft = false
                    Dialed.NOT_FOUND, Dialed.UNANSWERED -> Unit
                }
                // Made, perhaps made, or pending at the car: but for the last burst, the burst ends here.
                val knock = when (dialed) {
                    Dialed.KNOCKED -> "was made"
                    Dialed.UNSURE -> "may have been made"
                    Dialed.UNANSWERED -> "had no answer"
                    else -> null
                }
                if (knock != null && burst < most) {
                    // After one with no answer, the car's link is waited for as after any burst's last connect.
                    if (dialed == Dialed.UNANSWERED && carCame(came, times.knockSettleMillis)) return KnockEnd.CAME
                    pause = times.knockDialupPauseMillis
                    val detail = "$burst of $most, ended by the knock by dial-up, which $knock and brought no link, with no " +
                        "connect to the serial channel, so the next waits ${pause / 1000} s,"
                    log.line(diagnostics.record(LinkTry.Path.KNOCK_BURST, LinkTry.Outcome.NO_LINK, now() - burstBegan, detail = detail).describe())
                    continue
                }
            }
            // Whether the next connect is to the Serial Port service, and how long it waits.
            var toStandard = missing
            var wait = times.knockGapMillis
            for (attempt in 1..times.knockAttempts) {
                if (attempt > 1 && carCame(came, wait)) return KnockEnd.CAME
                if (deaf()) return KnockEnd.DEAF
                val standard = toStandard && standardLeft
                val to = if (standard) " $TO_STANDARD" else ""
                at.set("in burst $burst of $most, after connect $attempt$to")
                val knocked = knockOnce(address, came, "$burst.$attempt$to", if (standard) STANDARD_SERVICE_UUID else SERVICE_UUID)
                if (came.isCompleted) return KnockEnd.CAME
                if (knocked == Knocked.AWAY) return KnockEnd.AWAY
                if (probing && knocked == Knocked.UP) return if (carCame(came, times.knockSettleMillis)) KnockEnd.CAME else KnockEnd.CAME_UP
                toStandard = false
                wait = times.knockGapMillis
                if (standard) {
                    // One there at most, whatever came of it; one that failed, where the service is always there, may have
                    // run into another app's link.
                    standardLeft = false
                    inTheWay = knocked in FAILURES
                } else {
                    when (knocked) {
                        Knocked.FAILED_FOUND -> {
                            missing = false
                            inTheWay = true
                        }
                        Knocked.NOT_FOUND -> {
                            missing = true
                            toStandard = true
                        }
                        Knocked.UNSURE -> {
                            missing = true
                            toStandard = true
                            wait = times.knockUnsureWaitMillis
                        }
                        else -> Unit
                    }
                }
                if (inTheWay) break
            }
            if (carCame(came, times.knockSettleMillis)) return KnockEnd.CAME
            if (inTheWay && (burst == most || fallback == KnockFallback.ONCE_PER_KNOCK)) return KnockEnd.IN_THE_WAY
            pause = if (inTheWay) times.knockCollisionPauseMillis else times.knockPauseMillis
            val detail = if (inTheWay) {
                "$burst of $most, which ended after a connect that another app's link to the car may have been in the way of, " +
                    "so the next waits ${times.knockCollisionPauseMillis / 1000} s,"
            } else {
                "$burst of $most"
            }
            log.line(diagnostics.record(LinkTry.Path.KNOCK_BURST, LinkTry.Outcome.NO_LINK, now() - burstBegan, detail = detail).describe())
        }
        return KnockEnd.UNANSWERED
    }

    /** What a [probeKnock] came to, for what the connect does next. */
    private enum class ProbeEnd {
        /** The car's link came, and is the connect's. */
        TAKEN,

        /** None came: the connect dials. */
        NOTHING,

        /** None came, after a connect that may have run into another app's link: the connect dials after a pause. */
        IN_THE_WAY,
    }

    /**
     * The probing knock of a [connect] to [serial], whose agent is not known to knock: one burst ([knockAt]), with its
     * Serial Port connect as [fallback] allows, that ends after the first of its connects that came up, while the
     * connect listens for the car's link, which settles [won]. What it came to is kept in the diagnostics, in one line.
     * Where the car's link did not come, for whatever reason, the connect dials as it would have without it, after
     * [SerialConnectTimes.knockCollisionPauseMillis] where the probe ended after a connect that may have run into another
     * app's link. The phone cannot listen where [deaf] says so, and then it knocks no more. [dialup] is the knock by
     * dial-up's, as [knockAt]'s.
     */
    private suspend fun probeKnock(
        serial: SerialEndpoint,
        won: CompletableDeferred<Link>,
        fallback: KnockFallback,
        deaf: AtomicBoolean,
        dialup: Boolean,
    ): ProbeEnd {
        val began = now()
        val at = AtomicReference<String?>(null)
        fun line(outcome: LinkTry.Outcome, detail: String?) =
            log.line(diagnostics.record(LinkTry.Path.KNOCK_PROBE, outcome, now() - began, detail = detail).describe())
        val end = try {
            knockAt(serial.address, won, 1, fallback, at, probing = true, deaf = deaf::get, dialup = dialup)
        } catch (e: CancellationException) {
            // The car's link ends the connect, and with it the probe, as it does a knock; or the caller gave up.
            if (won.isCompleted && won.getCompletionExceptionOrNull() == null) line(LinkTry.Outcome.TAKEN, at.get()) else line(LinkTry.Outcome.CANCELLED, at.get())
            throw e
        }
        when (end) {
            KnockEnd.CAME -> {
                line(LinkTry.Outcome.TAKEN, at.get())
                return ProbeEnd.TAKEN
            }
            KnockEnd.AWAY -> line(LinkTry.Outcome.NO_LINK, "${at.get()}, which did not reach the car,")
            KnockEnd.CAME_UP -> line(LinkTry.Outcome.NO_LINK, "${at.get()}, which came up,")
            KnockEnd.UNANSWERED -> line(LinkTry.Outcome.NO_LINK, "of 1 burst")
            KnockEnd.DEAF -> line(LinkTry.Outcome.NO_LINK, "where the phone cannot listen for the car's link,")
            KnockEnd.IN_THE_WAY -> {
                line(LinkTry.Outcome.NO_LINK, "${at.get()}, $IN_THE_WAY_OF, so the phone dials after ${times.knockCollisionPauseMillis / 1000} s,")
                log.line("connect: the probing knock brought no link of the car's: the phone dials the agent after the pause")
                return ProbeEnd.IN_THE_WAY
            }
        }
        log.line("connect: the probing knock brought no link of the car's: the phone dials the agent as it would have")
        return ProbeEnd.NOTHING
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
     * It is made to [service]: the agent's, or the car's Serial Port service. What came of it ([Knocked]): it did not
     * reach the car ([Knocked.AWAY]) where it failed while the phone had no connection to the car
     * ([TryFacts.carConnected] false), or had no answer in its time while the phone had none or could not tell; then the
     * car is off or out of range, and the knock ends ([knock]). One that came up did, and so did one that failed where
     * the phone had found the service's channel, and one that failed while the phone was connected to the car or could
     * not tell. One that had no answer while the phone was connected to the car did too: the car is there, and leaves a
     * request without an answer at times, as after a dial of its own that failed ([Knocked.UNANSWERED]). One of the
     * others that did not come up failed: where the phone had found the channel ([Knocked.FAILED_FOUND]: the car refused
     * it, or another app's link to the car was in the way, which Android takes no second of, and which the failed connect
     * has closed), where it had found none ([Knocked.NOT_FOUND]: the car has no record of the service a phone can find,
     * and the connect went no further), or where it cannot tell ([Knocked.UNSURE]).
     */
    private suspend fun knockOnce(address: String, came: CompletableDeferred<Link>, which: String, service: UUID): Knocked {
        val began = now()
        var made: SerialSocket? = null
        var noted = false
        fun note(outcome: LinkTry.Outcome, failure: Throwable?, facts: TryFacts) {
            noted = true
            log.line(diagnostics.record(LinkTry.Path.KNOCK, outcome, now() - began, failure, facts, which).describe())
        }
        try {
            val socket = withContext(io) { radio.socket(address, service).also { made = it } }
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
            return Knocked.UP
        } catch (e: IOException) {
            val facts = facts(address, made, e)
            // The socket found the service's channel: the car turned the request down, or another app's link was in the way.
            val outcome = when {
                e is NoAnswer -> LinkTry.Outcome.NO_ANSWER
                facts.channel != null && facts.channel >= 0 -> LinkTry.Outcome.REFUSED
                else -> LinkTry.Outcome.FAILED
            }
            note(outcome, e, facts)
            return when {
                outcome == LinkTry.Outcome.REFUSED -> Knocked.FAILED_FOUND
                outcome == LinkTry.Outcome.NO_ANSWER -> if (facts.carConnected == true) Knocked.UNANSWERED else Knocked.AWAY
                facts.carConnected == false -> Knocked.AWAY
                facts.channel != null -> Knocked.NOT_FOUND
                else -> Knocked.UNSURE
            }
        } catch (e: CancellationException) {
            made?.closeQuietly()
            if (!noted) note(LinkTry.Outcome.CANCELLED, null, TryFacts())
            throw e
        }
    }

    /** What came of one of a knock's connects ([knockOnce]), for what the knock does next. */
    private enum class Knocked {
        /** It came up, and the knock's magic bytes were sent on it. */
        UP,

        /**
         * It failed where the phone had found the service's channel: the car refused it, or another app's link to the car
         * was in the way (Android takes no second link to the same channel, and the failed connect has closed that one).
         */
        FAILED_FOUND,

        /** It failed where the phone found no channel for the service: the car has no record of it that a phone finds. */
        NOT_FOUND,

        /** It failed, the car reached or may have been, where the phone cannot read whether it had found the channel. */
        UNSURE,

        /** It had no answer in its time while the phone was connected to the car. */
        UNANSWERED,

        /** It did not reach the car: the car is off or out of range, and the knock ends. */
        AWAY,
    }

    /**
     * A knock's connect to the car's dial-up networking service, the knock by dial-up ([knock]), [which] by its burst,
     * given [SerialConnectTimes.knockDialupTryMillis]. Nothing is written on it or read from it, and its socket is closed
     * once, however it ends: at once where it came up, which nobody should let it, as where it failed or had no answer;
     * and wherever it is where the knock is cancelled, as when the car's link has come. What came of it is kept in the
     * diagnostics, in one line.
     *
     * What came of it ([Dialed]): it did not reach the car ([Dialed.AWAY]) by the rule of [knockOnce]. One that failed
     * where the phone had found the service's channel was heard at the car, which or the agent refused it: the knock
     * was made ([Dialed.KNOCKED]). One where it found none found no such service on the car ([Dialed.NOT_FOUND]), and
     * one where it cannot tell may have made the knock ([Dialed.UNSURE]).
     */
    private suspend fun dialupOnce(address: String, which: String): Dialed {
        val began = now()
        var made: SerialSocket? = null
        val closed = AtomicBoolean(false)
        fun close() {
            if (closed.compareAndSet(false, true)) made?.closeQuietly()
        }
        var noted = false
        fun note(outcome: LinkTry.Outcome, failure: Throwable?, facts: TryFacts) {
            noted = true
            log.line(diagnostics.record(LinkTry.Path.KNOCK, outcome, now() - began, failure, facts, which).describe())
        }
        try {
            val socket = withContext(io) { radio.socket(address, DIALUP_UUID).also { made = it } }
            val up = withTimeoutOrNull(times.knockDialupTryMillis) {
                withContext(io) {
                    coroutineScope {
                        // Set once the connect has come back: from then on the socket is not the watcher's to close.
                        val settled = AtomicBoolean(false)
                        // Started here and now, as [bringUp]'s, so that a cancel before it ran still closes the socket.
                        val onCancel = launch(start = CoroutineStart.UNDISPATCHED) {
                            try {
                                awaitCancellation()
                            } finally {
                                if (!settled.get()) close()
                            }
                        }
                        try {
                            socket.connect()
                        } catch (e: IOException) {
                            // A connect that failed because it was cancelled, or ran out of its time, is no failure.
                            currentCoroutineContext().ensureActive()
                            throw e
                        } finally {
                            settled.set(true)
                        }
                        onCancel.cancel()
                    }
                }
            }
            val facts = facts(address, socket, if (up == null) NoAnswer(times.knockDialupTryMillis) else null)
            close()
            if (up == null) {
                note(LinkTry.Outcome.NO_ANSWER, NoAnswer(times.knockDialupTryMillis), facts)
                return if (facts.carConnected == true) Dialed.UNANSWERED else Dialed.AWAY
            }
            note(LinkTry.Outcome.DIALUP_UP, null, facts)
            return Dialed.UP
        } catch (e: IOException) {
            close()
            val facts = facts(address, made, e)
            val found = facts.channel != null && facts.channel >= 0
            val dialed = when {
                found -> Dialed.KNOCKED
                facts.carConnected == false -> Dialed.AWAY
                facts.channel != null -> Dialed.NOT_FOUND
                else -> Dialed.UNSURE
            }
            val outcome = when (dialed) {
                Dialed.KNOCKED -> LinkTry.Outcome.DIALUP_KNOCKED
                Dialed.NOT_FOUND -> LinkTry.Outcome.DIALUP_NONE
                Dialed.UNSURE -> LinkTry.Outcome.DIALUP_UNSURE
                else -> LinkTry.Outcome.FAILED
            }
            note(outcome, e, facts)
            return dialed
        } catch (e: CancellationException) {
            close()
            if (!noted) note(LinkTry.Outcome.CANCELLED, null, TryFacts())
            throw e
        }
    }

    /** What came of a knock's connect to the car's dial-up networking service ([dialupOnce]), for what the burst does next. */
    private enum class Dialed {
        /** It failed where the phone had found the service: the request was heard and refused, and the knock was made. */
        KNOCKED,

        /** It failed where the phone found no record of the service: the car does not have it on now. */
        NOT_FOUND,

        /** It failed, the car reached or may have been, where the phone cannot read whether it had found the service. */
        UNSURE,

        /** It had no answer in its time while the phone was connected to the car: the request is pending there. */
        UNANSWERED,

        /** It came up, which nobody should let it, and was closed at once. */
        UP,

        /** It did not reach the car: the car is off or out of range, and the knock ends. */
        AWAY,
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

        /**
         * Bluetooth's standard Serial Port service (0x1101), which the car advertises on the same RFCOMM channel as every
         * other serial service of its: what a knock goes through for a few connects after one to the agent's service
         * failed ([BluetoothSerialLinkConnector.knock]).
         */
        val STANDARD_SERVICE_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

        /** How a knock's connect to [STANDARD_SERVICE_UUID] is named in the diagnostics, after its place in the bursts. */
        private const val TO_STANDARD = "to the Serial Port service"

        /**
         * The car's dial-up networking service, the knock by dial-up's ([BluetoothSerialLinkConnector.knock]), on an
         * RFCOMM channel of its own, apart from the car's serial channel.
         */
        val DIALUP_UUID: UUID = UUID.fromString(ProtocolConstants.SERIAL_KNOCK_DIALUP_UUID)

        /** How a knock's connect to [DIALUP_UUID] is named in the diagnostics, after its burst. */
        private const val TO_DIALUP = "to the dial-up service"

        /** What the diagnostics say of a knock's connect that may have run into another app's link to the car. */
        private const val IN_THE_WAY_OF = "which another app's link to the car may have been in the way of"

        /** A knock's connect that failed: to the Serial Port service, any of these may have run into another app's link. */
        private val FAILURES = setOf(Knocked.FAILED_FOUND, Knocked.NOT_FOUND, Knocked.UNSURE)

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

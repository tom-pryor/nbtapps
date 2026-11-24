// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import android.util.Log
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlinx.serialization.json.JsonObject

/**
 * What the phone can tell of a try at a Bluetooth serial link beyond its exception: see [SerialRadio.inspect]. Each
 * is null where the phone would not say.
 */
data class TryFacts(
    /** `BluetoothSocketException.errorCode`, which Android has from version 14 (API 34). */
    val errorCode: Int? = null,
    /**
     * The RFCOMM channel the socket found on the car for the service it was opened to, the agent's or, for a knock's
     * connect that names it, the car's Serial Port service: a channel number where the car advertised it and the link
     * was refused or failed; -1 where the socket found none, which is either the car answering that it has no such
     * service or the phone not reaching the car to ask ([carConnected] is a hint of which).
     */
    val channel: Int? = null,
    /** The car had a hands-free or audio connection to the phone ([com.nbtapps.store.data.PhoneBluetooth.connected]). */
    val carConnected: Boolean? = null,
    /** The phone is paired with the car. */
    val bonded: Boolean? = null,
    /** The car had a media audio connection to the phone ([com.nbtapps.store.data.PhoneBluetooth.mediaAudio]). */
    val mediaAudio: Boolean? = null,
)

/** One try at a Bluetooth serial link, of either way it comes up, for the diagnostics. It holds no address. */
data class LinkTry(
    /** When it ended, in milliseconds since the epoch. */
    val at: Long,
    val path: Path,
    val outcome: Outcome,
    /**
     * How long it took, in milliseconds: for a link the car opened, its hand-over, which only writes the magic bytes;
     * for a listen-only connect, how long it listened.
     */
    val millis: Long? = null,
    /** The simple name of the exception's class, for one that failed. */
    val error: String? = null,
    val message: String? = null,
    val facts: TryFacts = TryFacts(),
    /**
     * Which of several it was, as a knock's burst and attempt, and where a knock's connect went to the car's Serial Port
     * service, that service ("1.2 to the Serial Port service"; one to the agent's own has no such words): words that
     * follow the path in [describe].
     */
    val detail: String? = null,
) {
    enum class Path(val label: String) {
        /** The phone dialled the car. */
        DIAL("dial"),

        /** The car opened the link to the phone. */
        ACCEPT("accept"),

        /** The phone began listening for the car's links. */
        LISTEN("listen"),

        /**
         * A connect that only listened for the car's link, and made no link of the phone's: its time is how long it
         * listened. See [BluetoothSerialLinkConnector].
         */
        LISTEN_ONLY("listen-only connect"),

        /**
         * One connect of a knock's: the phone knocked at the agent ([BluetoothSerialLinkConnector.knock]), through the
         * agent's own service or, where its [detail] says so, the car's Serial Port service.
         */
        KNOCK("knock"),

        /** A burst of a knock's connects that had no link from the car. */
        KNOCK_BURST("knock burst"),

        /**
         * A connect that knocked, and what came of it: the car's link, or none. Its time is from its start; its detail
         * names the connect the knocking was at, and its service where that was the car's Serial Port service.
         */
        KNOCK_CONNECT("knocking connect"),

        /**
         * A probing knock, made before the phone's dial to an agent not known to knock, and what came of it: the car's
         * link, or none, after which the phone dialled. Its time is from its start, and is what it added to the connect;
         * its detail is as [KNOCK_CONNECT]'s, or says that its connect that came up brought nothing.
         */
        KNOCK_PROBE("probing knock, to an agent not known to knock,"),
    }

    enum class Outcome(val label: String) {
        UP("up"),
        FAILED("failed"),

        /** The phone's dial heard nothing within its time, and was given up. */
        NO_ANSWER("no answer"),

        /** A link the car opened from a device the app was not waiting for. */
        TURNED_AWAY("turned away"),

        /** A listen-only connect took the car's link. */
        TAKEN("took the car's link"),

        /** A listen-only connect, or a knock's burst or connect, had no link from the car in its time. */
        NO_LINK("had no link from the car"),

        /** A knock's connect came up, and the knock's magic bytes were sent on it. */
        KNOCKED("came up, and the knock was sent"),

        /**
         * A knock's connect failed where the phone had found the service's channel on the car. The phone cannot tell why:
         * the car refused it, or another app on the phone has its link to the car's serial service, of which Android takes
         * no second, and which the failed connect has closed. Named for the first, as it was before the second was known.
         */
        REFUSED("failed with the service found (refused by the car, or another app's link to the car in the way)"),

        /** Given up because the connect it was part of was cancelled, or had its link. */
        CANCELLED("cancelled"),
    }

    /** One line, as the diagnostics and the log have it. */
    fun describe(): String = buildString {
        append(path.label)
        detail?.let { append(' ').append(it) }
        append(' ').append(outcome.label)
        millis?.let { append(" after ").append(it).append(" ms") }
        if (error != null) {
            append(": ").append(error)
            if (!message.isNullOrEmpty()) append(": ").append(message)
        }
        val known = listOfNotNull(
            facts.errorCode?.let { "error code $it" },
            facts.channel?.let { if (it < 0) "no channel" else "channel $it" },
            facts.carConnected?.let { if (it) "car connected" else "car not connected" },
            facts.bonded?.let { if (it) "paired" else "not paired" },
            facts.mediaAudio?.let { if (it) "media audio connected" else "no media audio" },
        )
        if (known.isNotEmpty()) append(" (").append(known.joinToString(", ")).append(')')
    }
}

/**
 * The last [capacity] tries at a Bluetooth serial link, of the phone's dial and of the car's links, kept in memory
 * only, for the user to copy ([report]) when a connect fails. Nothing here is written to disk or sent anywhere.
 */
class SerialDiagnostics(
    /** The app's version, Android's and the phone's model, in one line, as [report] begins with them. */
    private val about: String = "",
    /** Enough for the knocks of the app's own wait after an agent's restart, which come to several lines each. */
    private val capacity: Int = 200,
    private val wallClock: () -> Long = System::currentTimeMillis,
) {
    private val kept = ArrayDeque<LinkTry>()

    /** A try that ended now, as [LinkTry] with its time filled in: kept, and returned. */
    fun record(
        path: LinkTry.Path,
        outcome: LinkTry.Outcome,
        millis: Long? = null,
        failure: Throwable? = null,
        facts: TryFacts = TryFacts(),
        detail: String? = null,
    ): LinkTry {
        val entry = LinkTry(wallClock(), path, outcome, millis, failure?.let { it::class.java.simpleName }, failure?.message, facts, detail)
        synchronized(kept) {
            kept.addLast(entry)
            while (kept.size > capacity) kept.removeFirst()
        }
        return entry
    }

    /** The tries kept, oldest first. */
    fun tries(): List<LinkTry> = synchronized(kept) { kept.toList() }

    /**
     * Plain text for the user to copy: the app's version, Android's and the phone's model, the head unit's saved
     * [headUnitName], every try kept, and, where the app had a link to that head unit in this run, the agent's
     * `serial` object from its last `info.get` ([agentSerial]) as it came. No Bluetooth address is in it: any that
     * an exception's message or anything else brought is taken out.
     */
    fun report(headUnitName: String?, agentSerial: JsonObject?): String = withoutAddresses(
        buildString {
            appendLine("NBTApps Bluetooth diagnostics")
            if (about.isNotEmpty()) appendLine(about)
            headUnitName?.let { appendLine("Head unit: $it") }
            appendLine(
                if (agentSerial != null) "The agent's Bluetooth serial status, from its last info.get: $agentSerial"
                else "The agent's Bluetooth serial status: not read in this run",
            )
            val tries = tries()
            if (tries.isEmpty()) {
                appendLine("No tries in this run")
            } else {
                appendLine("The last ${tries.size} tries, oldest first:")
                tries.forEach { appendLine("${TIME.format(Instant.ofEpochMilli(it.at).atZone(ZoneId.systemDefault()))} ${it.describe()}") }
            }
        },
    )

    private companion object {
        val TIME: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss.SSS")
    }
}

/** Why no link came up, in words for the user, where the tries of one connect tell: see [causeOf]. */
enum class LinkCause {
    /** The car, which is connected to the phone, advertised no service of the agent's. */
    NO_SERVICE,

    /**
     * The phone found no service of the agent's, and the car is not connected to the phone over Bluetooth: the phone
     * may not have reached the car at all, so nothing is said of the agent.
     */
    NOT_CONNECTED,

    /** The car has the service and refused the link: its one link for phone apps is held by another app. */
    REFUSED,

    /** The car did not answer at all. */
    NO_ANSWER;

    /**
     * In words for the user, for an agent that [dials] or is not known to. Such an agent has the car dial a phone it
     * is paired with by itself, and one it is not paired with only while the pairing code is showing on the car.
     */
    fun text(dials: Boolean): String = when (this) {
        NO_SERVICE -> "The car has no NBTApp service on Bluetooth: the agent is probably not running, or the car's Bluetooth was only just switched on."
        NOT_CONNECTED -> "The car isn't connected to this phone over Bluetooth: it may be out of range, or its Bluetooth may be off."
        REFUSED -> "The car refused the link: its one link for phone apps is in use by another app, such as Android Auto." +
            if (dials) {
                " Keep this app open and the car connects to it by itself shortly. If this phone's pairing was " +
                    "forgotten on the car, it does so only while the pairing code is showing there."
            } else {
                ""
            }
        NO_ANSWER -> "The car didn't answer: it may be out of range."
    }
}

/**
 * What the phone's own [tries] of one connect tell of why none came up, or null where they do not. A try that heard
 * nothing within its time, or that Android says timed out, was not answered; one that found a channel was refused.
 * One that found no channel says the car has no such service where the car is connected to the phone, and that the
 * car is not connected where it is not, as the phone may then never have reached it; where the phone could not tell
 * whether the car is connected, that try says nothing. The last try that says anything counts.
 */
fun causeOf(tries: List<LinkTry>): LinkCause? {
    for (attempt in tries.asReversed()) {
        if (attempt.path != LinkTry.Path.DIAL || attempt.outcome == LinkTry.Outcome.UP) continue
        val channel = attempt.facts.channel
        return when {
            attempt.outcome == LinkTry.Outcome.NO_ANSWER || attempt.facts.errorCode == L2CAP_TIMEOUT -> LinkCause.NO_ANSWER
            channel != null && channel < 0 -> when (attempt.facts.carConnected) {
                true -> LinkCause.NO_SERVICE
                false -> LinkCause.NOT_CONNECTED
                null -> continue
            }
            channel != null -> LinkCause.REFUSED
            else -> continue
        }
    }
    return null
}

/** `BluetoothSocketException.L2CAP_TIMEOUT`. */
private const val L2CAP_TIMEOUT = 14

private val BLUETOOTH_ADDRESS = Regex("(?<![0-9A-Fa-f])([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])")

/** [text] with every Bluetooth address in it replaced: for what the user copies, and for the log. */
fun withoutAddresses(text: String): String = BLUETOOTH_ADDRESS.replace(text, "<address>")

/** The app's lines about its links to head units: see [AndroidLinkLog]. They never hold a Bluetooth address. */
fun interface LinkLog {
    fun line(text: String)

    companion object {
        /** Says nothing: for tests, which have no Android log. */
        val NONE = LinkLog {}
    }
}

/** [LinkLog] to Android's log, under one tag, with any address taken out ([withoutAddresses]). */
object AndroidLinkLog : LinkLog {
    const val TAG = "NbtLink"

    override fun line(text: String) {
        Log.i(TAG, withoutAddresses(text))
    }
}

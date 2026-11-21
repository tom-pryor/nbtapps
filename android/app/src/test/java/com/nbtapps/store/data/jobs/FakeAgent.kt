// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.crypto.newSha256
import com.nbtapps.store.data.hmacSha256
import com.nbtapps.store.data.json.StrictJson
import com.nbtapps.store.data.link.Frame
import com.nbtapps.store.data.link.FrameCodec
import com.nbtapps.store.data.link.FrameReader
import com.nbtapps.store.data.link.IncomingLink
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.security.SecureRandom
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.zip.Deflater
import kotlin.concurrent.thread
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive

/** A log file a [FakeAgent] lists and sends: see [FakeAgent.logs]. */
class FakeLog(val path: String, val bytes: ByteArray, val modified: Long = 1_794_400_000)

/**
 * Thrown by a [FakeAgent] handler to answer a request with an error. [extra] is JSON text of more members of the
 * error object, after its code and message, each with a comma before it: `,"keys":[...]` for `invalid`.
 */
class Refusal(val code: String, val extra: String = "") : Exception(code)

/**
 * A package's settings as a [FakeAgent] gives them with `settings.get` and changes them with `settings.set`: the
 * options as its manifest has them, and how each stands in its file. It outlives a link, as the head unit's file does,
 * so a test gives the same one to every agent of a connector. It checks no value: a test says what it refuses.
 */
class FakeSettings(
    val packageId: String,
    /** The options, each as the JSON text of its manifest's object for it, in the manifest's order. */
    val options: List<String>,
) {
    /** The JSON text of the value of each option that is set. */
    val values = ConcurrentHashMap<String, String>()

    /** The raw text of each option set by hand to a value its type does not take. */
    val byHand = ConcurrentHashMap<String, String>()

    /** A `state` to send for an option in place of the one it has, such as one of a later version. */
    val states = ConcurrentHashMap<String, String>()

    /** How many options one answer of `settings.get` holds at most. Null for all of them. */
    @Volatile
    var pageSize: Int? = null

    /** The code `settings.get` is refused with, or null to answer. */
    @Volatile
    var refuseGet: String? = null

    /** The code `settings.set` is refused with, or null to take it: after [setsBeforeRefusal] that were taken. */
    @Volatile
    var refuseSet: String? = null

    @Volatile
    var setsBeforeRefusal = 0

    /** Keys whose values `settings.set` refuses as `invalid`, with the reason. */
    val faults = ConcurrentHashMap<String, String>()

    /** The params of every `settings.set`, refused or taken, on every link. */
    val sets = CopyOnWriteArrayList<JsonObject>()

    private fun key(option: String): String = StrictJson.parseObject(option.encodeToByteArray()).getValue("key").jsonPrimitive.content

    fun get(params: JsonObject): String {
        refuseGet?.let { throw Refusal(it) }
        if (params["package"]?.jsonPrimitive?.content != packageId) throw Refusal("not-found")
        val from = params["from"]?.jsonPrimitive?.content?.toInt() ?: 0
        if (from >= options.size) throw Refusal("bad-request")
        val page = options.drop(from).take(pageSize ?: options.size)
        val next = (from + page.size).takeIf { it < options.size }?.let { ""","next":$it""" }.orEmpty()
        return page.joinToString(",", """{"options":[""", "]$next}") { option ->
            val key = key(option)
            val state = states[key]?.let { ""","state":"$it"""" }
                ?: values[key]?.let { ""","state":"set","value":$it""" }
                ?: byHand[key]?.let { ""","state":"by-hand","raw":"$it"""" }
                ?: ""","state":"unset""""
            option.trimEnd().removeSuffix("}") + state + "}"
        }
    }

    fun set(params: JsonObject): String {
        sets += params
        if (params["package"]?.jsonPrimitive?.content != packageId) throw Refusal("not-found")
        refuseSet?.let { code -> if (sets.size > setsBeforeRefusal) throw Refusal(code) }
        val set = params["set"] as? JsonObject ?: JsonObject(emptyMap())
        val reset = (params["reset"] as? JsonArray)?.map { it.jsonPrimitive.content }.orEmpty()
        val known = options.map(::key)
        val wrong = (set.keys + reset).mapNotNull { key ->
            val reason = if (key !in known) "unknown-key" else faults[key]
            reason?.let { """{"key":"$key","reason":"$it"}""" }
        }
        if (wrong.isNotEmpty()) {
            throw Refusal("invalid", wrong.joinToString(",", ""","keys":[""", "]"))
        }
        for ((key, value) in set) {
            values[key] = value.toString()
            byHand.remove(key)
            states.remove(key)
        }
        for (key in reset) {
            values.remove(key)
            byHand.remove(key)
            states.remove(key)
        }
        return "{}"
    }
}

/**
 * What an agent that asks for `auth` knows of phones: the keys it issued, as the real agent's phones file holds
 * them, and its pairing window. It outlives a link, so a test gives the same one to every agent of a connector
 * ([FakeConnector.phones]).
 */
class FakePhones {
    /** Phone id to the key issued to it. */
    val keys = ConcurrentHashMap<String, ByteArray>()

    /** The code the car's screen shows while its pairing window is open, or null with none open. */
    @Volatile
    var code: String? = null

    /** The agent proves itself with another key than the one it issued: what it does when the two sides' keys differ. */
    @Volatile
    var provesWrong = false

    /** How many keys were issued over `pairing.issue`. */
    val issued = AtomicInteger()

    /** Pairs the phone [phoneId] as if it had been done before, and returns the key. */
    fun pair(phoneId: String): ByteArray = newKey().also { keys[phoneId] = it }

    /** What "Forget all phones" on the car's screen does. */
    fun forgetAll() = keys.clear()

    fun newKey(): ByteArray = ByteArray(32).also { SecureRandom().nextBytes(it) }
}

/**
 * A problem one of a [FakeChecks]' checks finds. [found] is what its fix removes, and [moves] the folders it moves
 * aside, from and to; [fixLabel] null for a check that has no fix for it. The paths are made up: nothing here is a
 * real head unit's.
 */
class FakeCheck(
    val id: String,
    @Volatile var found: List<String>,
    val title: String = "Something was put onto the head unit by hand",
    val detail: String = "It stands in the way.",
    @Volatile var moves: List<Pair<String, String>> = emptyList(),
    val fixLabel: String? = "Remove them",
    val fixDetail: String = "Removes what was listed, a path at a time.",
    val restart: String = "hmi-restart",
)

/**
 * What a [FakeAgent]'s checks find, and what its fixes did: `checks.run` and `checks.fix` as the agent has them,
 * with a token over each list. It outlives a link, as the head unit's files do, so a test gives the same one to
 * every agent of a connector. With no problems, which is how it starts, every check is ok.
 */
class FakeChecks {
    /** The checks, in the order the agent answers them. */
    @Volatile
    var ids = listOf("system-writable", "aa-fix")

    /** The problems there are, by check id. A check with none is ok. */
    val problems = ConcurrentHashMap<String, FakeCheck>()

    /** The paths one answer of `checks.run` holds at most, as the link's frame bounds them. Null for all. */
    @Volatile
    var pageSize: Int? = null

    /** The code `checks.run` is refused with, or null to answer. */
    @Volatile
    var refuseRun: String? = null

    /** The code the fix of a check is refused with, by its id, after the token has been compared. */
    val refuseFix = ConcurrentHashMap<String, String>()

    /**
     * The paths a fix leaves, by check id: those of its list that are still there afterwards, and the folders it
     * could not move aside, by where they are.
     */
    val leaves = ConcurrentHashMap<String, List<String>>()

    /** Called after each answer of `checks.run`, as a head unit's files may change between two pages. */
    @Volatile
    var afterRun: () -> Unit = {}

    /** How many `checks.run` requests were answered, on every link. */
    val runs = AtomicInteger()

    /** Every fix asked for, on every link: the check's id and the token sent. */
    val fixes = CopyOnWriteArrayList<Pair<String, String>>()

    fun add(problem: FakeCheck) {
        problems[problem.id] = problem
    }

    /** The token of [paths] and [moves] for the check [id]: as the agent's, a hash over all, so another list has another. */
    fun token(id: String, paths: List<String>, moves: List<Pair<String, String>> = emptyList()): String =
        Hex.encode(newSha256().digest((id + "\n" + paths.sorted().joinToString("\n") + "\n" + moves.joinToString("\n")).encodeToByteArray()))

    private fun token(problem: FakeCheck) = token(problem.id, problem.found, problem.moves)

    private fun moves(moves: List<Pair<String, String>>) = moves.joinToString(",", "[", "]") { """{"from":"${it.first}","to":"${it.second}"}""" }

    /** The answer of `checks.run` with [params]. */
    fun run(params: JsonObject): String {
        refuseRun?.let { throw Refusal(it) }
        val one = params["id"]?.jsonPrimitive?.content
        val after = params["after"]?.jsonPrimitive?.content
        if (one != null && one !in ids) throw Refusal("not-found")
        var room = pageSize ?: Int.MAX_VALUE
        val answer = (if (one != null) listOf(one) else ids).joinToString(",", """{"checks":[""", "]}") { id ->
            val problem = problems[id] ?: return@joinToString """{"id":"$id","state":"ok"}"""
            val all = problem.found.sorted()
            val from = if (after == null) all else all.filter { it > after }
            val page = from.take(room)
            room -= page.size
            // Once one list is cut short, the ones after it wait for their own pages.
            if (page.size < from.size) room = 0
            val fix = problem.fixLabel?.let { label ->
                ""","fix":{"label":"$label","detail":"${problem.fixDetail}","restart":"${problem.restart}","token":"${token(problem)}"}"""
            }.orEmpty()
            """{"id":"$id","state":"problem","title":"${problem.title}","detail":"${problem.detail}","count":${all.size},""" +
                """"found":${page.joinToString(",", "[", "]") { "\"$it\"" }},"more":${page.size < from.size},""" +
                """"moves":${moves(problem.moves)}$fix}"""
        }
        runs.incrementAndGet()
        afterRun()
        return answer
    }

    /** The answer of `checks.fix` with [params]; [agent] is the one asked, whose restart mark a fix raises. */
    fun fix(params: JsonObject, agent: FakeAgent): String {
        val id = params["id"]?.jsonPrimitive?.content ?: throw Refusal("bad-request")
        val token = params["token"]?.jsonPrimitive?.content ?: throw Refusal("bad-request")
        fixes += id to token
        if (id !in ids) throw Refusal("not-found")
        val problem = problems[id] ?: return if (token == token(id, emptyList())) FIXED else throw Refusal("stale")
        if (problem.fixLabel == null) throw Refusal("unsupported")
        if (token != token(problem)) throw Refusal("stale")
        refuseFix[id]?.let { throw Refusal(it) }
        val leaving = leaves[id].orEmpty()
        val left = leaving.filter { it in problem.found }
        val stayed = problem.moves.filter { it.first in leaving }
        val moved = problem.moves - stayed.toSet()
        val removed = problem.found.size - left.size
        if (left.isEmpty() && stayed.isEmpty()) {
            problems.remove(id)
        } else {
            problem.found = left
            problem.moves = stayed
        }
        if (problem.restart != "none") agent.restartNeeded = problem.restart
        val state = if (left.isEmpty() && stayed.isEmpty()) "ok" else "problem"
        val notMoved = stayed.map { it.first }
        return """{"state":"$state","removed":$removed,"moved":${moves(moved)},"left":${(notMoved + left).joinToString(",", "[", "]") { "\"$it\"" }},""" +
            """"leftMore":0,"stopped":["demod"],"restart":"${problem.restart}"}"""
    }

    private companion object {
        const val FIXED = """{"state":"ok","removed":0,"moved":[],"left":[],"leftMore":0,"stopped":[],"restart":"none"}"""
    }
}

/**
 * A channel of an installed package that a [FakeAgent] has: its program sends back each message it is given, as
 * the agent repo's `chan_echo` does, upper-cased with [upper]. With [switched] it is off until `channel.set` switches
 * it on. With [readOnly] (the manifest's `access: read`) a message from the phone closes it with `protocol`.
 */
class FakeChannel(
    val packageId: String,
    val name: String,
    val switched: Boolean = false,
    val readOnly: Boolean = false,
    val upper: Boolean = false,
    val version: String = "1.0.0",
) {
    @Volatile
    var on: Boolean = !switched
}

/**
 * A stand-in for the head-unit agent that answers what a test tells it to. It speaks real frames over a
 * [MemoryLink], so the app's whole stack runs against it; what it lacks is any behaviour of its own. Each
 * request is handled on a thread of its own, so a handler may itself fetch a blob from the phone.
 *
 * One thing it does as the real agent does, when it is given [phones]: it answers nothing but `hello`, `ping`,
 * `auth` and `pairing.issue` until the phone has proved a pairing key, issues a key for the code of an open
 * pairing window, and ends a link on which `auth` failed. Without [phones] it is an agent that asks for no
 * `auth`, and has no such methods.
 *
 * It also sends files as the agent does for `files.fetch`, from [logs]: the answer, and then the DATA frames of
 * one raw deflate stream, [dataChunk] bytes a frame. A test can have a stream stop until it is cancelled, end with
 * `files.error`, or come damaged.
 *
 * And it answers `serial.reset` as the agent does ([serialReset]), with the car ending the link after `sent`.
 *
 * With `channels` among its [capabilities] it has the channels a test adds to [channels], each an echo
 * ([FakeChannel]): `channel.open`, `channel.get` and `channel.set` as the agent has them, a message of the phone's
 * sent back within the credit the phone granted, and a grant back for each message its program took, as the real
 * agent gives when its program keeps up. A test can close a channel ([closeChannel]) and pause them all
 * ([pauseChannels]); what the phone said of its channels is in [channelEvents].
 */
class FakeAgent(private val link: Link, val headUnitId: String) {
    /** See the class. Set before the phone's first byte arrives. */
    @Volatile
    var phones: FakePhones? = null

    /** Whether the phone has proved its key on this link. */
    @Volatile
    var trusted = false
        private set

    private var phoneId: String? = null

    /** SHA-256 of the `hello` that came and of the answer that went, as bytes: what the proofs of `auth` are over. */
    @Volatile
    private var greeting: ByteArray? = null

    /** The proof the phone owes once it has the agent's nonce. */
    @Volatile
    private var proofDue: ByteArray? = null

    /** Method name to the JSON text of its result. A method with no handler is answered `unsupported`. */
    val handlers = ConcurrentHashMap<String, (JsonObject) -> String>()

    /** Every request received, in order: the method and its params. */
    val requests = CopyOnWriteArrayList<Pair<String, JsonObject>>()

    /** The methods it takes and never answers, as an agent whose answer is lost. */
    val silent: MutableSet<String> = ConcurrentHashMap.newKeySet()

    /** The JSON array text `packages.list` answers with. */
    @Volatile
    var installed = "[]"

    @Volatile
    var restartNeeded = "none"

    /** The JSON array text of the capabilities `hello` lists. */
    @Volatile
    var capabilities = "[]"

    /** The largest frame `hello` gives. */
    @Volatile
    var maxFrame = 65536

    /**
     * The settings `settings.get` gives and `settings.set` changes, as an agent that lists `settings` has them. One
     * that does not list it answers both `unsupported`, as the real one does.
     */
    @Volatile
    var settings: FakeSettings? = null

    /** What `checks.run` finds and `checks.fix` removes: see [FakeChecks]. Nothing is found unless a test says so. */
    @Volatile
    var checks = FakeChecks()

    /** The log files `files.list` gives and `files.fetch` sends, by package id, newest first. */
    val logs = ConcurrentHashMap<String, List<FakeLog>>()

    /** The bytes of one DATA frame of a files stream, at most. */
    @Volatile
    var dataChunk = 1000

    /** After this many DATA frames a files stream sends no more until it is cancelled. */
    @Volatile
    var stallAfterFrames: Int? = null

    /** After this many DATA frames a files stream ends with `files.error`. */
    @Volatile
    var errorAfterFrames: Int? = null

    /** The paths whose deflate streams come with a byte of them changed. */
    val damaged: MutableSet<String> = ConcurrentHashMap.newKeySet()

    /**
     * What `serial.reset` answers, as the agent does for an agent that lists `serial-reset`: `sent`, `armed`, or the
     * code of a refusal. One that does not list it answers `unsupported`, as the real one does.
     */
    @Volatile
    var serialReset = "sent"

    /**
     * With `sent`, the car ends the link this long after the answer, as the real agent has it do, which for the phone
     * is the link ending from the far side. Null where the car leaves it up.
     */
    @Volatile
    var resetEndsAfterMs: Long? = 300

    /** How many `serial.reset` requests were taken, refusals among them. */
    val resets = AtomicInteger()

    /** The channels it has, by "package name". See the class. */
    val channels = ConcurrentHashMap<String, FakeChannel>()

    /** Every `channel.credit` and `channel.close` the phone sent, as it came. */
    val channelEvents = LinkedBlockingQueue<JsonObject>()

    /** Every message the phone sent on a channel, by stream, as it came. */
    val channelMessages = ConcurrentHashMap<Int, CopyOnWriteArrayList<ByteArray>>()

    fun addChannel(channel: FakeChannel) {
        channels["${channel.packageId} ${channel.name}"] = channel
    }

    /** One open channel: what its program has to send back, and what the phone has granted. Guarded by itself. */
    private class OpenChannel(val spec: FakeChannel, val stream: Int, var phoneGranted: Int) {
        val back = ArrayDeque<ByteArray>()
        var closed = false
    }

    private val openChannels = ConcurrentHashMap<Int, OpenChannel>()
    private val nextChannel = AtomicInteger(ProtocolConstants.CHANNEL_FIRST_STREAM)

    /** The streams of the channels open now. */
    fun openChannelStreams(): Set<Int> = openChannels.keys.toSet()

    /** Closes the channel [stream] as the agent does, telling the phone [reason]. */
    fun closeChannel(stream: Int, reason: String) {
        val open = openChannels.remove(stream) ?: return
        synchronized(open) { open.closed = true }
        send("""{"event":"channel.close","params":{"stream":$stream,"reason":"$reason"}}""")
    }

    /** What a job does to every channel: `channel.paused`, or `channel.resumed`. */
    fun pauseChannels(paused: Boolean) = send("""{"event":"${if (paused) "channel.paused" else "channel.resumed"}","params":{}}""")

    private fun hasChannels() = "\"${ProtocolConstants.CAPABILITY_CHANNELS}\"" in capabilities

    private fun channelOf(params: JsonObject): FakeChannel {
        if (!hasChannels()) throw Refusal("unsupported")
        val packageId = params["package"]?.jsonPrimitive?.content ?: throw Refusal("bad-request")
        val name = params["channel"]?.jsonPrimitive?.content ?: throw Refusal("bad-request")
        return channels["$packageId $name"] ?: throw Refusal("not-found")
    }

    private fun switchOf(channel: FakeChannel): String {
        val open = openChannels.values.count { it.spec === channel }
        return """{"switched":${channel.switched},"on":${channel.on},"open":$open}"""
    }

    /** A DATA frame of the phone's on a channel: sent back, as the program does, within what the phone granted. */
    private fun channelData(frame: Frame) {
        val open = openChannels[frame.stream] ?: return
        channelMessages.getOrPut(frame.stream) { CopyOnWriteArrayList() } += frame.payload
        if (open.spec.readOnly || !frame.last || frame.payload.isEmpty()) return closeChannel(frame.stream, "protocol")
        val text = if (open.spec.upper) frame.payload.decodeToString().uppercase().encodeToByteArray() else frame.payload
        synchronized(open) { open.back.addLast(text) }
        // The program took it: the phone may send one more.
        send("""{"event":"channel.credit","params":{"stream":${frame.stream},"messages":1}}""")
        flushChannel(open)
    }

    private fun flushChannel(open: OpenChannel) = synchronized(open) {
        while (!open.closed && open.phoneGranted > 0 && open.back.isNotEmpty()) {
            open.phoneGranted--
            val message = open.back.removeFirst()
            synchronized(link) { link.output.write(FrameCodec.encode(FrameCodec.TYPE_DATA, FrameCodec.FLAG_LAST, open.stream, message)) }
        }
    }

    /** The phone's `channel.credit` and `channel.close`. */
    private fun channelEvent(message: JsonObject) {
        channelEvents.put(message)
        val params = message["params"] as? JsonObject ?: return
        val stream = params["stream"]?.jsonPrimitive?.content?.toIntOrNull() ?: return
        val open = openChannels[stream] ?: return
        when (message["event"]?.jsonPrimitive?.content) {
            "channel.credit" -> {
                synchronized(open) { open.phoneGranted += params["messages"]?.jsonPrimitive?.content?.toIntOrNull() ?: 0 }
                flushChannel(open)
            }
            "channel.close" -> {
                openChannels.remove(stream)
                synchronized(open) { open.closed = true }
            }
        }
    }

    /** A files stream that is being sent: its frames go out with its lock held, so a cancel comes after them all. */
    private class Outgoing {
        var cancelled = false
        val stopped = CountDownLatch(1)
    }

    private val outgoing = ConcurrentHashMap<Int, Outgoing>()
    private val nextStream = AtomicInteger(1)

    private val reader = FrameReader(link)
    private val answers = ConcurrentHashMap<Int, LinkedBlockingQueue<JsonObject>>()
    private val streams = ConcurrentHashMap<Int, LinkedBlockingQueue<Frame>>()
    private val events = LinkedBlockingQueue<JsonObject>()
    private var nextId = 1

    /** Open until the phone has closed its side of the link, with this end still open. */
    private val left = CountDownLatch(1)

    init {
        handlers["hello"] = {
            """{"protocolMin":1,"protocolMax":1,"role":"agent","headUnitId":"$headUnitId","agentVersion":"1.0.0","maxFrame":$maxFrame,"capabilities":$capabilities}"""
        }
        handlers["settings.get"] = { params ->
            val known = settings?.takeIf { "\"${ProtocolConstants.CAPABILITY_SETTINGS}\"" in capabilities } ?: throw Refusal("unsupported")
            known.get(params)
        }
        handlers["settings.set"] = { params ->
            val known = settings?.takeIf { "\"${ProtocolConstants.CAPABILITY_SETTINGS}\"" in capabilities } ?: throw Refusal("unsupported")
            known.set(params)
        }
        handlers["info.get"] = {
            """{"agentVersion":"1.0.0","headUnitId":"$headUnitId","manifestVersion":1,""" +
                """"free":{"app":1000,"data":2000,"plugins":3000,"staging":4000},"restartNeeded":"$restartNeeded"}"""
        }
        handlers["packages.list"] = { """{"packages":$installed,"more":false}""" }
        handlers["job.status"] = { """{"events":[],"more":false}""" }
        handlers["ping"] = { "{}" }
        handlers["checks.run"] = { params -> checks.run(params) }
        handlers["checks.fix"] = { params -> checks.fix(params, this) }
        handlers["files.list"] = { params ->
            val files = logs[params["package"]?.jsonPrimitive?.content] ?: throw Refusal("not-found")
            files.joinToString(",", """{"files":[""", """],"more":false}""") { """{"path":"${it.path}","size":${it.bytes.size},"modified":${it.modified}}""" }
        }
        handlers["files.cancel"] = { params ->
            outgoing[params.getValue("stream").jsonPrimitive.content.toInt()]?.let { stream ->
                synchronized(stream) { stream.cancelled = true }
                stream.stopped.countDown()
            }
            "{}"
        }
        handlers["serial.reset"] = {
            if ("\"${ProtocolConstants.CAPABILITY_SERIAL_RESET}\"" !in capabilities) throw Refusal("unsupported")
            resets.incrementAndGet()
            when (serialReset) {
                "sent" -> {
                    resetEndsAfterMs?.let { after ->
                        thread(isDaemon = true) {
                            Thread.sleep(after)
                            link.close()
                        }
                    }
                    """{"state":"sent"}"""
                }
                "armed" -> """{"state":"armed","forMs":60000}"""
                else -> throw Refusal(serialReset)
            }
        }
        handlers["channel.open"] = { params ->
            val channel = channelOf(params)
            if (!channel.on) throw Refusal("off")
            if (openChannels.size >= ProtocolConstants.CHANNEL_PER_LINK) throw Refusal("too-many")
            val credit = params["credit"]?.jsonPrimitive?.content?.toInt() ?: ProtocolConstants.CHANNEL_CREDIT
            val stream = nextChannel.getAndIncrement()
            openChannels[stream] = OpenChannel(channel, stream, credit)
            """{"stream":$stream,"maxMessage":${ProtocolConstants.CHANNEL_MAX_MESSAGE},"credit":${ProtocolConstants.CHANNEL_CREDIT},"version":"${channel.version}"}"""
        }
        handlers["channel.get"] = { params -> switchOf(channelOf(params)) }
        handlers["channel.set"] = { params ->
            val channel = channelOf(params)
            if (!channel.switched) throw Refusal("bad-request")
            channel.on = params["on"]?.jsonPrimitive?.content == "true"
            if (!channel.on) openChannels.values.filter { it.spec === channel }.forEach { closeChannel(it.stream, "off") }
            switchOf(channel)
        }
        thread(isDaemon = true, name = "fake-agent") { run() }
    }

    fun requestsOf(method: String): List<JsonObject> = requests.filter { it.first == method }.map { it.second }

    /** Sends an event to the phone. */
    fun event(name: String, seq: Long, params: String) = send("""{"event":"$name","seq":$seq,"params":$params}""")

    /**
     * Says `link.closing` with [reason], as the agent does before it ends a link of its own accord: an event with
     * no `seq`. The link is left open, as the real agent leaves its end until the phone has gone: [phoneLeft].
     */
    fun closing(reason: String) = send("""{"event":"link.closing","params":{"reason":"$reason"}}""")

    /** Waits for the phone to end the link, and says whether it did. */
    fun phoneLeft(): Boolean = left.await(5, TimeUnit.SECONDS)

    /** Fetches a range of a blob from the phone, as the agent does, and returns its bytes. */
    fun fetch(hash: String, offset: Long, length: Long? = null): ByteArray {
        val reply = ask("blob.fetch", """{"sha256":"$hash","offset":$offset${length?.let { ""","length":$it""" } ?: ""}}""")
        val result = reply["result"] as? JsonObject ?: throw Refusal((reply.getValue("error") as JsonObject).getValue("code").toString().trim('"'))
        val stream = result.getValue("stream").toString().toInt()
        val frames = streams.getOrPut(stream) { LinkedBlockingQueue() }
        val out = ByteArrayOutputStream()
        while (true) {
            val frame = frames.poll(10, TimeUnit.SECONDS) ?: throw IOException("the stream stopped")
            out.write(frame.payload)
            if (frame.last) break
        }
        check(out.size().toString() == result.getValue("size").toString()) { "the stream is not as long as its reply said" }
        return out.toByteArray()
    }

    /** The next event the phone sent, such as `blob.error`. */
    fun phoneEvent(): JsonObject = events.poll(10, TimeUnit.SECONDS) ?: throw IOException("the phone sent no event")

    /** Ends the link, as a head unit going out of range does. */
    fun drop() = link.close()

    private fun ask(method: String, params: String): JsonObject {
        val id = synchronized(this) { nextId++ }
        val answer = answers.getOrPut(id) { LinkedBlockingQueue() }
        send("""{"id":$id,"method":"$method","params":$params}""")
        return answer.poll(10, TimeUnit.SECONDS) ?: throw IOException("the phone did not answer $method")
    }

    private fun send(json: String) = synchronized(link) {
        link.output.write(FrameCodec.encode(FrameCodec.TYPE_MSG, 0, 0, json.encodeToByteArray()))
    }

    private fun run() {
        try {
            if (!reader.raw(4).contentEquals(FrameCodec.MAGIC)) return link.close()
            while (true) {
                val frame = reader.next() ?: return left.countDown()
                if (frame.type == FrameCodec.TYPE_DATA) {
                    if (frame.stream >= ProtocolConstants.CHANNEL_FIRST_STREAM) channelData(frame) else streams.getOrPut(frame.stream) { LinkedBlockingQueue() }.put(frame)
                    continue
                }
                val message = StrictJson.parseObject(frame.payload)
                val method = message["method"]?.toString()?.trim('"')
                val event = message["event"]?.toString()?.trim('"')
                when {
                    event == "channel.credit" || event == "channel.close" -> channelEvent(message)
                    method != null -> {
                        val id = message.getValue("id").toString()
                        val params = message["params"] as? JsonObject ?: JsonObject(emptyMap())
                        requests += method to params
                        thread(isDaemon = true) { answer(id, method, params, frame.payload) }
                    }
                    message["event"] != null -> events.put(message)
                    else -> answers.getOrPut(message.getValue("id").toString().toInt()) { LinkedBlockingQueue() }.put(message)
                }
            }
        } catch (e: IOException) {
            // The link ended.
        }
    }

    /** [payload] is the request as it came: `hello`'s is hashed for `auth`. */
    private fun answer(id: String, method: String, params: JsonObject, payload: ByteArray) {
        if (method in silent) return
        try {
            val known = phones
            if (method == "files.fetch" && !handlers.containsKey(method) && (known == null || trusted)) return sendFile(id, params)
            val result = when {
                known != null && method == "auth" -> auth(known, params)
                known != null && method == "pairing.issue" -> issue(known, params)
                known != null && !trusted && method != "hello" && method != "ping" -> throw Refusal("unauthenticated")
                else -> (handlers[method] ?: throw Refusal("unsupported"))(params)
            }
            val reply = """{"id":$id,"result":$result}"""
            if (method == "hello") {
                phoneId = params["phoneId"]?.jsonPrimitive?.content
                // Before the answer goes: the phone's `auth` follows it at once.
                greeting = newSha256().digest(payload) + newSha256().digest(reply.encodeToByteArray())
            }
            send(reply)
        } catch (e: Refusal) {
            try {
                send("""{"id":$id,"error":{"code":"${e.code}","message":"the fake agent says ${e.code}"${e.extra}}}""")
                // As the real agent does: a link on which `auth` failed is ended once the answer is out.
                if (e.code == "auth-failed") link.close()
            } catch (closed: IOException) {
                // The link ended meanwhile.
            }
        } catch (e: IOException) {
            // The link ended meanwhile.
        }
    }

    /** `files.fetch` from [logs]: the answer, then the stream's DATA frames, as the test has them go. */
    private fun sendFile(id: String, params: JsonObject) {
        try {
            val log = logs[params["package"]?.jsonPrimitive?.content]?.firstOrNull { it.path == params["path"]?.jsonPrimitive?.content }
                ?: throw Refusal("not-found")
            val offset = params["offset"]?.jsonPrimitive?.content?.toInt() ?: 0
            if (offset > log.bytes.size) throw Refusal("out-of-range")
            val length = params["length"]?.jsonPrimitive?.content?.toInt() ?: Int.MAX_VALUE
            val bytes = log.bytes.copyOfRange(offset, offset + minOf(length, log.bytes.size - offset))
            val compress = params["compress"]?.jsonPrimitive?.content ?: throw Refusal("bad-request")
            if (bytes.isEmpty()) return send("""{"id":$id,"result":{"size":0,"compress":"$compress"}}""")
            val payload = if (compress == "deflate") deflate(bytes) else bytes
            if (log.path in damaged) payload[payload.size / 2] = (payload[payload.size / 2].toInt() xor 0x55).toByte()
            val number = nextStream.getAndIncrement()
            val stream = Outgoing().also { outgoing[number] = it }
            send("""{"id":$id,"result":{"stream":$number,"size":${bytes.size},"compress":"$compress"}}""")
            val frames = (payload.indices step dataChunk).map { payload.copyOfRange(it, minOf(it + dataChunk, payload.size)) }
            for ((i, frame) in frames.withIndex()) {
                if (i == stallAfterFrames) stream.stopped.await(10, TimeUnit.SECONDS)
                synchronized(stream) {
                    if (stream.cancelled) return
                    if (i == errorAfterFrames) {
                        send("""{"event":"files.error","params":{"stream":$number,"code":"io-error","message":"read failed"}}""")
                        return
                    }
                    synchronized(link) {
                        link.output.write(FrameCodec.encode(FrameCodec.TYPE_DATA, if (i == frames.size - 1) FrameCodec.FLAG_LAST else 0, number, frame))
                    }
                }
            }
        } catch (e: Refusal) {
            send("""{"id":$id,"error":{"code":"${e.code}","message":"the fake agent says ${e.code}"}}""")
        }
    }

    private fun deflate(bytes: ByteArray): ByteArray {
        val deflater = Deflater(6, true)
        deflater.setInput(bytes)
        deflater.finish()
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(8192)
        while (!deflater.finished()) out.write(buffer, 0, deflater.deflate(buffer))
        deflater.end()
        return out.toByteArray()
    }

    /** `auth`, in its two requests: the phone's nonce, answered with the agent's and its proof; then the phone's proof. */
    private fun auth(known: FakePhones, params: JsonObject): String {
        val hashes = greeting ?: throw Refusal("bad-state")
        val nonce = params["nonce"]?.jsonPrimitive?.content
        if (nonce != null) {
            if (proofDue != null) throw Refusal("bad-state")
            val theirs = Hex.decodeOrNull(nonce, ProtocolConstants.AUTH_NONCE_BYTES) ?: throw Refusal("bad-request")
            val key = known.keys[phoneId] ?: throw Refusal("auth-failed")
            val mine = ByteArray(ProtocolConstants.AUTH_NONCE_BYTES).also { SecureRandom().nextBytes(it) }
            val transcript = theirs + mine + hashes
            proofDue = hmacSha256(key, "nbt-auth-2".encodeToByteArray() + transcript)
            val shown = if (known.provesWrong) known.newKey() else key
            return """{"nonce":"${Hex.encode(mine)}","proof":"${Hex.encode(hmacSha256(shown, "nbt-auth-1".encodeToByteArray() + transcript))}"}"""
        }
        val proof = params["proof"]?.jsonPrimitive?.content ?: throw Refusal("bad-request")
        val due = proofDue ?: throw Refusal("bad-state")
        if (proof != Hex.encode(due)) throw Refusal("auth-failed")
        trusted = true
        return "{}"
    }

    /** `pairing.issue`: a key for the code of the open window, which closes with it. */
    private fun issue(known: FakePhones, params: JsonObject): String {
        val open = known.code ?: throw Refusal("forbidden")
        val code = params["code"]?.jsonPrimitive?.content
        if (code == null || code.length != ProtocolConstants.PAIRING_CODE_DIGITS || !code.all { it in '0'..'9' }) throw Refusal("bad-request")
        if (code != open) throw Refusal("rejected")
        known.code = null
        val key = known.pair(requireNotNull(phoneId))
        known.issued.incrementAndGet()
        return """{"key":"${Hex.encode(key)}"}"""
    }
}

/**
 * Gives the app a link to a new [FakeAgent] each time it connects. A test sets each one up in [onConnect]. It
 * reaches TCP endpoints, or with [serial] Bluetooth serial ones, over a link that says it is serial. With [serial]
 * the car can open a link too, to an agent set up the same way, while the app listens: [dialIn].
 */
class FakeConnector(
    private val headUnitId: String = "000102030405060708090a0b0c0d0e0f",
    private val serial: Boolean = false,
) : LinkConnector {
    val agents = CopyOnWriteArrayList<FakeAgent>()

    /** When set, every agent from here on asks for `auth`, and they share these phones: see [FakeAgent.phones]. */
    @Volatile
    var phones: FakePhones? = null

    /** Runs on each new agent before the phone's first byte reaches it. */
    @Volatile
    var onConnect: (FakeAgent) -> Unit = {}

    /** When set, connecting fails with this message. */
    @Volatile
    var unreachable: String? = null

    val agent: FakeAgent get() = agents.last()

    /** Whether each connect was told that the head unit's agent dials, in order. */
    val dials = CopyOnWriteArrayList<Boolean>()

    override suspend fun connect(endpoint: Endpoint, dials: Boolean): Link {
        this.dials += dials
        return connect(endpoint)
    }

    override fun supports(endpoint: Endpoint): Boolean = if (serial) endpoint is SerialEndpoint else endpoint is TcpEndpoint

    /**
     * A call of [accept] that waits: the endpoints it takes a link from, the link it is handed, and whether its caller
     * took that link, which one that was cancelled just then did not.
     */
    private class Listener(val endpoints: Collection<Endpoint>) {
        val handed = CompletableDeferred<IncomingLink>()
        val taken = CompletableFuture<Boolean>()
    }

    /** The calls of [accept] that wait now. */
    private val listeners = CopyOnWriteArrayList<Listener>()

    /** How many calls of [accept] wait now: whether the app listens for the car. */
    val listening = MutableStateFlow(0)

    /** How many times [accept] has been called. */
    val listens = AtomicInteger()

    /** When set, listening cannot start, and says this. */
    @Volatile
    var listenFailure: String? = null

    override fun accepts(endpoint: Endpoint): Boolean = serial && endpoint is SerialEndpoint

    @OptIn(ExperimentalCoroutinesApi::class)
    override suspend fun accept(endpoints: Collection<Endpoint>): IncomingLink {
        listens.incrementAndGet()
        listenFailure?.let { throw IOException(it) }
        val waiter = Listener(endpoints)
        listeners += waiter
        listening.update { it + 1 }
        try {
            return waiter.handed.await().also { waiter.taken.complete(true) }
        } catch (e: CancellationException) {
            // As the app's listener: a link handed over just as the caller gave up is closed, not lost open.
            if (!waiter.handed.completeExceptionally(e) && waiter.handed.getCompletionExceptionOrNull() == null) {
                waiter.handed.getCompleted().link.close()
            }
            waiter.taken.complete(false)
            throw e
        } finally {
            listeners -= waiter
            listening.update { it - 1 }
        }
    }

    /**
     * The car at [address] opens a link to the phone, to a new agent set up by [onConnect], as one the phone dialled
     * is. Null where the app does not listen for that car now, or gave up listening just then, which closes the link
     * before anything is written on it. Waits for the app to take the link or close it.
     */
    fun dialIn(address: String): FakeAgent? {
        for (waiter in listeners) {
            val endpoint = waiter.endpoints.firstOrNull { it is SerialEndpoint && it.address.equals(address, ignoreCase = true) } ?: continue
            val (phone, other) = MemoryLink.pair(kind = Link.SERIAL)
            val agent = FakeAgent(other, headUnitId).also {
                it.phones = phones
                onConnect(it)
            }
            if (!waiter.handed.complete(IncomingLink(endpoint, phone))) {
                phone.close()
                continue
            }
            if (!waiter.taken.get(10, TimeUnit.SECONDS)) continue
            agents += agent
            return agent
        }
        return null
    }

    override suspend fun connect(endpoint: Endpoint): Link {
        unreachable?.let { throw IOException(it) }
        val (phone, other) = MemoryLink.pair(kind = if (serial) Link.SERIAL else "memory")
        // Nothing is read until the phone writes, and it writes only once this has returned.
        agents += FakeAgent(other, headUnitId).also {
            it.phones = phones
            onConnect(it)
        }
        return phone
    }
}

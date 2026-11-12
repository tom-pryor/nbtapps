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
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive

/** Thrown by a [FakeAgent] handler to answer a request with an error. */
class Refusal(val code: String) : Exception(code)

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
 * A stand-in for the head-unit agent that answers what a test tells it to. It speaks real frames over a
 * [MemoryLink], so the app's whole stack runs against it; what it lacks is any behaviour of its own. Each
 * request is handled on a thread of its own, so a handler may itself fetch a blob from the phone.
 *
 * One thing it does as the real agent does, when it is given [phones]: it answers nothing but `hello`, `ping`,
 * `auth` and `pairing.issue` until the phone has proved a pairing key, issues a key for the code of an open
 * pairing window, and ends a link on which `auth` failed. Without [phones] it is an agent that asks for no
 * `auth`, and has no such methods.
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

    /** The JSON array text `packages.list` answers with. */
    @Volatile
    var installed = "[]"

    @Volatile
    var restartNeeded = "none"

    private val reader = FrameReader(link)
    private val answers = ConcurrentHashMap<Int, LinkedBlockingQueue<JsonObject>>()
    private val streams = ConcurrentHashMap<Int, LinkedBlockingQueue<Frame>>()
    private val events = LinkedBlockingQueue<JsonObject>()
    private var nextId = 1

    /** Open until the phone has closed its side of the link, with this end still open. */
    private val left = CountDownLatch(1)

    init {
        handlers["hello"] = {
            """{"protocolMin":1,"protocolMax":1,"role":"agent","headUnitId":"$headUnitId","agentVersion":"1.0.0","maxFrame":65536,"capabilities":[]}"""
        }
        handlers["info.get"] = {
            """{"agentVersion":"1.0.0","headUnitId":"$headUnitId","manifestVersion":1,""" +
                """"free":{"app":1000,"data":2000,"plugins":3000,"staging":4000},"restartNeeded":"$restartNeeded"}"""
        }
        handlers["packages.list"] = { """{"packages":$installed,"more":false}""" }
        handlers["job.status"] = { """{"events":[],"more":false}""" }
        handlers["ping"] = { "{}" }
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
                    streams.getOrPut(frame.stream) { LinkedBlockingQueue() }.put(frame)
                    continue
                }
                val message = StrictJson.parseObject(frame.payload)
                val method = message["method"]?.toString()?.trim('"')
                when {
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
        try {
            val known = phones
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
                send("""{"id":$id,"error":{"code":"${e.code}","message":"the fake agent says ${e.code}"}}""")
                // As the real agent does: a link on which `auth` failed is ended once the answer is out.
                if (e.code == "auth-failed") link.close()
            } catch (closed: IOException) {
                // The link ended meanwhile.
            }
        } catch (e: IOException) {
            // The link ended meanwhile.
        }
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
 * reaches TCP endpoints, or with [serial] Bluetooth serial ones, over a link that says it is serial.
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

    override fun supports(endpoint: Endpoint): Boolean = if (serial) endpoint is SerialEndpoint else endpoint is TcpEndpoint

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

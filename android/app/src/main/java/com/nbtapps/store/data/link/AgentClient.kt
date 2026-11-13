// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.PairingKeys
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.crypto.newSha256
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.FileArea
import com.nbtapps.store.model.FileFetch
import com.nbtapps.store.model.FileListing
import com.nbtapps.store.model.Hello
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.InstalledPage
import com.nbtapps.store.model.JobLog
import com.nbtapps.store.model.JobResult
import com.nbtapps.store.model.JobSnapshot
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.PlanRequest
import com.nbtapps.store.model.TrustedSource
import com.nbtapps.store.model.TrustedSources
import java.io.IOException
import java.security.MessageDigest
import java.security.SecureRandom
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.encodeToJsonElement
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

/** The agent answered, but not with what the protocol says it answers. The message is shown to the user. */
open class ProtocolException(message: String) : IOException(message)

/** In `auth`, the agent's proof was not the one the phone's pairing key gives: the two no longer hold the same key. */
class AgentProofException : ProtocolException("The head unit doesn't hold this phone's pairing key")

/** One event of a job, with the sequence number the agent gave it. */
sealed interface JobEvent {
    val seq: Long

    /** The job the event belongs to. */
    val planId: String

    data class Progress(override val seq: Long, val job: JobSnapshot) : JobEvent {
        override val planId: String get() = job.planId
    }

    data class Log(override val seq: Long, val log: JobLog) : JobEvent {
        override val planId: String get() = log.planId
    }

    data class Done(override val seq: Long, val result: JobResult) : JobEvent {
        override val planId: String get() = result.planId
    }

    companion object {
        /** The event [name] with [params], or null if the app does not know it or cannot read it: such events are ignored. */
        fun parse(name: String, seq: Long?, params: JsonObject): JobEvent? {
            val number = seq ?: return null
            return try {
                when (name) {
                    "job.progress" -> Progress(number, AgentClient.json.decodeFromJsonElement(params))
                    "job.log" -> Log(number, AgentClient.json.decodeFromJsonElement(params))
                    "job.done" -> Done(number, AgentClient.json.decodeFromJsonElement(params))
                    else -> null
                }
            } catch (e: SerializationException) {
                null
            } catch (e: IllegalArgumentException) {
                null
            }
        }
    }
}

/** The result of `job.status`. */
class JobStatus(
    /** The running job or, failing that, the last one. Null with no job on record. */
    val job: JobSnapshot?,
    /** The `job.log` and `job.done` events numbered above `after`, oldest first. */
    val events: List<JobEvent>,
    /** Events remain: ask again with `after` set to the last one received. */
    val more: Boolean,
)

/**
 * The agent's methods as functions. Each sends one request over [peer] and reads the answer; an error
 * response comes out as [RemoteException] with the protocol's code.
 */
class AgentClient(
    private val peer: Peer,
    /** The largest frame this side takes on the link, which `hello` offers: lower on Bluetooth serial. */
    private val offeredFrame: Int = FrameCodec.MAX_PAYLOAD,
    /** Called once `hello` has been answered by an agent this app can talk to. */
    private val onHello: () -> Unit = {},
) {
    /** SHA-256 of the `hello` this side sent and of the answer it got, as they travelled: what `auth` proves over. */
    @Volatile
    private var greeting: ByteArray? = null

    /** What the agent listed in its `hello` as its capabilities: the names in [ProtocolConstants]. Empty until then. */
    @Volatile
    var capabilities: List<String> = emptyList()
        private set

    /**
     * Opens the conversation. The phone dialled, so it sends `hello` first; the answer carries the agent's
     * values. Sets the link's frame size to the lower of the two.
     *
     * @throws ProtocolException if the other side is not an agent this app can talk to.
     */
    suspend fun hello(phoneId: String, phoneName: String): Hello {
        val mine = Hello(
            protocolMin = PROTOCOL_VERSION,
            protocolMax = PROTOCOL_VERSION,
            role = "phone",
            phoneId = phoneId,
            name = phoneName.substring(0, phoneName.offsetByCodePoints(0, minOf(MAX_NAME_CHARS, phoneName.codePointCount(0, phoneName.length)))),
            maxFrame = offeredFrame,
        )
        val theirs: Hello = try {
            val said = peer.exchange("hello", json.encodeToJsonElement(mine).jsonObject)
            greeting = newSha256().digest(said.sent) + newSha256().digest(said.received)
            decode<Hello>(said.result)
        } catch (e: RemoteException) {
            if (e.code == "unsupported-version") throw ProtocolException("The head unit's agent speaks a version of the protocol this app doesn't")
            throw e
        } catch (e: ProtocolException) {
            // An answer that is no `hello` at all is refused as one with wrong values is: nothing more is said here.
            peer.close(e.message ?: "The head unit sent an answer this app can't read")
            throw e
        }
        val refusal = when {
            theirs.role != "agent" -> "The other side isn't a head-unit agent"
            minOf(theirs.protocolMax, PROTOCOL_VERSION) < maxOf(theirs.protocolMin, PROTOCOL_VERSION) ->
                "The head unit's agent speaks a version of the protocol this app doesn't"
            theirs.maxFrame !in FrameCodec.MIN_PAYLOAD..FrameCodec.MAX_PAYLOAD -> "The head unit's agent asked for a frame size that isn't allowed"
            theirs.headUnitId?.let(::isId) != true -> "The head unit's agent gave no valid id"
            theirs.agentVersion == null -> "The head unit's agent gave no version"
            else -> null
        }
        if (refusal != null) {
            peer.close(refusal)
            throw ProtocolException(refusal)
        }
        peer.maxFrame = minOf(theirs.maxFrame, offeredFrame)
        capabilities = theirs.capabilities
        onHello()
        return theirs
    }

    /**
     * Proves to the agent that this phone holds the pairing key it issued, and has the agent prove the same, in
     * two requests. On Bluetooth serial the agent tells a phone nothing until this is done.
     *
     * The phone sends its nonce A and gets the agent's nonce B with the agent's proof; it checks that, and sends
     * its own. Both proofs are HMAC-SHA256 under the key, of a label and then
     * `T = A || B || SHA-256(hello request) || SHA-256(hello reply)`, the two hashes being of the messages as
     * they travelled: `nbt-auth-1` is the agent's label and `nbt-auth-2` the phone's. So a proof is good for
     * this link's `hello` only, and neither side's can be played back as the other's.
     *
     * [mac] is the HMAC under the key: the key itself is not to be had ([com.nbtapps.store.data.PairingKeys]).
     *
     * @throws RemoteException with `auth-failed` if the agent has no key for this phone or took the proof for
     *   wrong; it then closes the link. With `unsupported` or `forbidden` if it asks for no `auth` on this link.
     * @throws AgentProofException if the agent's own proof is wrong. Nothing more is sent.
     */
    suspend fun auth(nonce: ByteArray = newNonce(), mac: suspend (ByteArray) -> ByteArray) {
        val hashes = greeting ?: throw ProtocolException("The head unit has to be greeted before anything is proved to it")
        require(nonce.size == ProtocolConstants.AUTH_NONCE_BYTES) { "a nonce is ${ProtocolConstants.AUTH_NONCE_BYTES} bytes" }
        val answer = peer.request("auth", buildJsonObject { put("nonce", Hex.encode(nonce)) })
        val theirNonce = answer["nonce"].stringOrNull()?.let { Hex.decodeOrNull(it, ProtocolConstants.AUTH_NONCE_BYTES) }
        val theirProof = answer["proof"].stringOrNull()?.let { Hex.decodeOrNull(it, PROOF_BYTES) }
        if (theirNonce == null || theirProof == null) throw ProtocolException("The head unit sent an answer this app can't read")
        val transcript = nonce + theirNonce + hashes
        // Compared in the same time whatever differs.
        if (!MessageDigest.isEqual(mac(AGENT_LABEL + transcript), theirProof)) throw AgentProofException()
        peer.request("auth", buildJsonObject { put("proof", Hex.encode(mac(PHONE_LABEL + transcript))) })
    }

    /**
     * Pairs this phone: hands the agent the code its pairing window shows on the car's screen, and returns the
     * 32-byte key the agent then made for this phone. It crosses the link once, here.
     *
     * @throws RemoteException with `forbidden` if no window is open, `rejected` for a wrong code, `bad-request`
     *   for what is no code at all.
     */
    suspend fun pairingIssue(code: String): ByteArray {
        val result: JsonObject = call("pairing.issue", buildJsonObject { put("code", code) })
        return result["key"].stringOrNull()?.let { Hex.decodeOrNull(it, PairingKeys.KEY_BYTES) }
            ?: throw ProtocolException("The head unit sent an answer this app can't read")
    }

    suspend fun info(): AgentInfo = call("info.get")

    /** Every installed package, sorted by id. The agent sends a long list in pages; this asks for them all. */
    suspend fun packages(): List<InstalledPackage> {
        val all = ArrayList<InstalledPackage>()
        while (true) {
            val page: InstalledPage = call(
                "packages.list",
                buildJsonObject { all.lastOrNull()?.let { put("after", it.id) } },
            )
            all += page.packages
            // An empty page that claims more would never end.
            if (!page.more || page.packages.isEmpty()) return all
        }
    }

    suspend fun sources(): List<TrustedSource> = call<TrustedSources>("sources.list").sources

    /** Asks for a plan. The answer comes once the agent has fetched the manifests it needs, which takes as long as the link does. */
    suspend fun plan(request: PlanRequest): Plan = call("job.plan", json.encodeToJsonElement(request).jsonObject)

    /** Runs the plan. Sending it twice is harmless, and sending it for a paused job resumes that job over this link. */
    suspend fun start(planId: String): JobSnapshot = jobOf(call<JsonObject>("job.start", planIdParams(planId)))
        ?: throw ProtocolException("The head unit started the job but didn't say where it stands")

    /** Stops a job that is still fetching. Fails with `too-late` once the install has begun. */
    suspend fun cancel(planId: String) {
        call<JsonObject>("job.cancel", planIdParams(planId))
    }

    /** The job with [planId], or the running or last one. With [after], also the log and done events numbered above it. */
    suspend fun status(planId: String? = null, after: Long? = null): JobStatus {
        val result: JsonObject = call(
            "job.status",
            buildJsonObject {
                planId?.let { put("planId", it) }
                after?.let { put("after", it) }
            },
        )
        val events = (result["events"] as? JsonArray).orEmpty().mapNotNull { item ->
            val event = item as? JsonObject ?: return@mapNotNull null
            JobEvent.parse(event["event"].stringOrNull().orEmpty(), event["seq"].integerOrNull(), event["params"] as? JsonObject ?: return@mapNotNull null)
        }
        val more = (result["more"] as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull ?: false
        return JobStatus(jobOf(result), events, more)
    }

    /** [kind] is `hmi` or `reboot`. The agent answers and then restarts. */
    suspend fun restart(kind: String) {
        call<JsonObject>("system.restart", buildJsonObject { put("kind", kind) })
    }

    /**
     * Whether the agent is started with the head unit, and whether it can be there. Only an agent that lists
     * [ProtocolConstants.CAPABILITY_AUTOSTART] has this and [setAutostart]: another answers `unsupported`.
     */
    suspend fun autostart(): Autostart = call("autostart.get")

    /**
     * Switches the start with the head unit on or off, and returns how things then stand.
     *
     * @throws RemoteException with one of [com.nbtapps.store.model.AutostartReason]'s where it cannot be switched
     *   on, and with `busy` from an agent that is on trial after its own update.
     */
    suspend fun setAutostart(on: Boolean): Autostart = call("autostart.set", buildJsonObject { put("on", on) })

    /** The files of the package [packageId] in [area], newest first: see [FileListing]. Only an agent that lists `files` has this. */
    suspend fun filesList(packageId: String, area: String = FileArea.LOGS): FileListing =
        call("files.list", buildJsonObject {
            put("package", packageId)
            put("area", area)
        })

    /**
     * Has the agent send the file [path] of the package [packageId], from byte [offset] for [length] bytes or to
     * its end, in the way [compress] says. Its DATA frames go to [sink] as they come, the first maybe before this
     * returns. With a size of 0 there is no stream, and [sink] is told nothing.
     *
     * @throws RemoteException with `not-found` for a path that `files.list` does not give now, `out-of-range` for
     *   an offset past the end, `busy` while a job runs or another stream is open on the link.
     */
    suspend fun filesFetch(
        packageId: String,
        path: String,
        compress: String,
        sink: StreamSink,
        offset: Long? = null,
        length: Long? = null,
        area: String = FileArea.LOGS,
    ): FileFetch {
        val result = peer.requestStream(
            "files.fetch",
            buildJsonObject {
                put("package", packageId)
                put("area", area)
                put("path", path)
                offset?.let { put("offset", it) }
                length?.let { put("length", it) }
                put("compress", compress)
            },
            sink,
        )
        val fetch = try {
            decode<FileFetch>(result)
        } catch (e: ProtocolException) {
            result["stream"].integerOrNull()?.let { peer.endStream(it.toInt()) }
            throw e
        }
        if (fetch.size < 0 || fetch.compress != compress || (fetch.stream == null) != (fetch.size == 0L)) {
            fetch.stream?.let(peer::endStream)
            throw ProtocolException("The head unit sent an answer this app can't read")
        }
        return fetch
    }

    /**
     * Stops the stream [stream] of a `files.fetch`. The answer comes after the stream's last frame that the agent
     * sends, so once this returns nothing more comes on it. A stream that has ended already is answered the same.
     */
    suspend fun filesCancel(stream: Int) {
        call<JsonObject>("files.cancel", buildJsonObject { put("stream", stream) })
        peer.endStream(stream)
    }

    private fun planIdParams(planId: String) = buildJsonObject { put("planId", planId) }

    private fun jobOf(result: JsonObject): JobSnapshot? = (result["job"] as? JsonObject)?.let { decode(it) }

    private suspend inline fun <reified T> call(method: String, params: JsonObject = JsonObject(emptyMap())): T =
        decode(peer.request(method, params))

    private inline fun <reified T> decode(value: JsonObject): T = try {
        json.decodeFromJsonElement(value)
    } catch (e: SerializationException) {
        throw ProtocolException("The head unit sent an answer this app can't read")
    } catch (e: IllegalArgumentException) {
        throw ProtocolException("The head unit sent an answer this app can't read")
    }

    companion object {
        const val PROTOCOL_VERSION = ProtocolConstants.PROTOCOL_VERSION
        const val MAX_NAME_CHARS = 64

        /** A proof of `auth`: an HMAC-SHA256. */
        private const val PROOF_BYTES = 32
        private val AGENT_LABEL = "nbt-auth-1".encodeToByteArray()
        private val PHONE_LABEL = "nbt-auth-2".encodeToByteArray()

        /** A nonce for `auth`: used once, so from the system's random source. */
        fun newNonce(): ByteArray = ByteArray(ProtocolConstants.AUTH_NONCE_BYTES).also { SecureRandom().nextBytes(it) }

        /** Unknown fields are ignored, and a field with no value is left out: null is never sent. */
        internal val json = Json {
            ignoreUnknownKeys = true
            explicitNulls = false
            encodeDefaults = true
        }

        /** A head-unit or phone id: 32 lowercase hex characters. */
        fun isId(text: String): Boolean = text.length == 32 && text.all { it in '0'..'9' || it in 'a'..'f' }
    }
}

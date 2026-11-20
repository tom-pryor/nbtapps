// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobKeeper
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.RemoteException
import com.nbtapps.store.data.link.monotonicMillis
import com.nbtapps.store.model.ChannelCloseReason
import com.nbtapps.store.model.ChannelError
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Why the port went off by itself. The user's own switching off has none. */
enum class PortOffReason {
    /** [SshPort.IDLE_MS] with no connection open. */
    IDLE,

    /** The link to the head unit was lost. */
    LINK_LOST,

    /** SSH access was switched off on the head unit. */
    SWITCHED_OFF,

    /** A job installed, upgraded or removed the SSH package. */
    PACKAGE_CHANGED,
}

/** An address of this phone that a client can reach the port at, and what kind of network it is on. */
data class PortAddress(val kind: Kind, val host: String) {
    enum class Kind {
        /** 127.0.0.1: an SSH app on this phone. */
        THIS_PHONE,

        /** The phone's own hotspot, as its interface's name says. */
        HOTSPOT,

        /** The Wi-Fi the phone is on, as its interface's name says. */
        WIFI,

        /** Another network, or one whose kind its name does not say. */
        NETWORK,
    }
}

/** How the port stands. Each state but the first [Off] is of the head unit [headUnitId], by this app's id. */
sealed interface SshPortState {
    val headUnitId: String?

    /** The port listens, or is being switched on: the link is held for it, and the foreground service runs for it. */
    val active: Boolean get() = this is Checking || this is On

    /**
     * The port is off. [reason] where it went off by itself, [failure] (a line for the user) where it could not be
     * switched on; neither where it was never on, or the user switched it off.
     */
    data class Off(override val headUnitId: String? = null, val reason: PortOffReason? = null, val failure: String? = null) : SshPortState

    /** A tunnel is opened to the head unit as a check, before the port listens. */
    data class Checking(override val headUnitId: String) : SshPortState

    /**
     * The port listens on [port]: on 127.0.0.1, and with [otherDevices] on every address of the phone. [connections]
     * are open or being opened; [paused] while a job runs on the head unit, which holds them. [addresses] are where a
     * client reaches it, this phone's first. [hostKey] is the fingerprint of the head unit's ed25519 host key, from
     * the check. [otherDevicesFailure] says why other devices could not be let in, where they could not.
     */
    data class On(
        override val headUnitId: String,
        val port: Int,
        val otherDevices: Boolean = false,
        val connections: Int = 0,
        val paused: Boolean = false,
        val addresses: List<PortAddress> = listOf(PortAddress(PortAddress.Kind.THIS_PHONE, SshPort.LOOPBACK)),
        val hostKey: String,
        val otherDevicesFailure: String? = null,
    ) : SshPortState
}

/** A tunnel as the port uses one: [SshTunnel]'s two streams, its state and host keys, and a close from any thread. */
interface PortTunnel {
    val input: InputStream
    val output: OutputStream
    val state: StateFlow<ChannelState>
    val hostKeys: List<String>

    /** Closes it, from any thread; closing one that has ended does nothing. */
    fun close()
}

/** How SSH access stands on the head unit, as `channel.get` says. */
enum class SwitchReading { ON, OFF, GONE, UNKNOWN }

/** The head unit's SSH channel as the port uses it: [SshChannel], or a test's. */
interface PortChannel {
    /** [SshTunnel.open] with [keys]. @throws SshTunnelException or another [IOException]. */
    suspend fun open(session: AgentSession, keys: List<String>): PortTunnel

    /** Whether SSH access is on: [SwitchReading.UNKNOWN] where the agent could not say, as during a job. */
    suspend fun switch(session: AgentSession): SwitchReading
}

/** The real one: [SshTunnel] over the session, and the agent's `channel.get`. */
object SshChannel : PortChannel {
    override suspend fun open(session: AgentSession, keys: List<String>): PortTunnel {
        val tunnel = SshTunnel.open(session, keys)
        return object : PortTunnel {
            override val input: InputStream get() = tunnel.input
            override val output: OutputStream get() = tunnel.output
            override val state: StateFlow<ChannelState> get() = tunnel.state
            override val hostKeys: List<String> get() = tunnel.hostKeys

            override fun close() = tunnel.closeBlocking()
        }
    }

    override suspend fun switch(session: AgentSession): SwitchReading = try {
        if (session.client.channelGet(SshTunnel.PACKAGE, SshTunnel.CHANNEL).on) SwitchReading.ON else SwitchReading.OFF
    } catch (e: RemoteException) {
        if (e.code == ChannelError.NOT_FOUND) SwitchReading.GONE else SwitchReading.UNKNOWN
    } catch (e: IOException) {
        SwitchReading.UNKNOWN
    }
}

/** What the port needs of the app's link to a head unit: [JobRepository]'s ([JobPortLink]), or a test's. */
interface PortLink {
    val connection: StateFlow<ConnectionState>

    /** The head unit a job runs on now, by this app's id; null where none does. */
    val jobOn: Flow<String?>

    /** [block] with the link to [headUnitId], as a use of it: [JobRepository.usingSession]. */
    suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T>
}

/** The app's link, [JobRepository]'s, as the port uses it. */
class JobPortLink(private val jobs: JobRepository) : PortLink {
    override val connection: StateFlow<ConnectionState> get() = jobs.connection
    override val jobOn: Flow<String?> = jobs.job.map { view -> view?.takeIf { !it.ended && !it.linkLost && !it.forgotten }?.headUnitId }

    override suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T> = jobs.usingSession(headUnitId, block)
}

/**
 * Opens the port's listening socket: on 127.0.0.1, or with [allDevices] on every address of the phone, IPv4 and IPv6,
 * the mobile data network's included.
 */
fun interface PortSockets {
    /** @throws IOException where [port] cannot be had. */
    fun listen(allDevices: Boolean, port: Int): ServerSocket
}

/**
 * The real sockets. Android's sockets are IPv6 ones that take IPv4 too, and a bind to `0.0.0.0` on them is the IPv6
 * wildcard `::`: with [allDevices] the port is reachable at every address of the phone, IPv4 and IPv6, on Wi-Fi, the
 * hotspot and mobile data alike, wherever the network lets a connection in.
 */
object PlainPortSockets : PortSockets {
    override fun listen(allDevices: Boolean, port: Int): ServerSocket {
        val socket = ServerSocket()
        try {
            socket.reuseAddress = true
            val address = if (allDevices) InetAddress.getByName("0.0.0.0") else InetAddress.getByName(SshPort.LOOPBACK)
            socket.bind(InetSocketAddress(address, port), BACKLOG)
            return socket
        } catch (e: IOException) {
            socket.close()
            throw e
        }
    }

    private const val BACKLOG = 8
}

/**
 * The IPv4 addresses of the phone's interfaces that are up and not loopback, each with its kind as the interface's
 * name tells it: `wlan0` is the Wi-Fi the phone is on; `ap`, `swlan`, `softap` and another `wlan` the phone's hotspot;
 * the rest another network.
 */
fun phoneAddresses(): List<PortAddress> {
    val interfaces = try {
        NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
    } catch (e: SocketException) {
        emptyList()
    }
    return interfaces.filter { runCatching { it.isUp && !it.isLoopback }.getOrDefault(false) }.flatMap { network ->
        val kind = interfaceKind(network.name)
        network.inetAddresses.toList().filterIsInstance<Inet4Address>().map { PortAddress(kind, it.hostAddress ?: "") }
    }.filter { it.host.isNotEmpty() }.sortedBy { it.kind.ordinal }
}

/** The kind of network an interface of [name] is on, as Android names its interfaces. */
internal fun interfaceKind(name: String): PortAddress.Kind = when {
    name == "wlan0" -> PortAddress.Kind.WIFI
    name.startsWith("ap") || name.startsWith("swlan") || name.startsWith("softap") || name.startsWith("wlan") -> PortAddress.Kind.HOTSPOT
    else -> PortAddress.Kind.NETWORK
}

/**
 * Whether the app still needs the local network permission (`android.permission.ACCESS_LOCAL_NETWORK`) to let other
 * devices in: from Android 17 an app that targets it takes no connection from the local network without it, and
 * neither the socket nor the app is told. [sdk] is the phone's Android, [targetSdk] the app's, [granted] whether it
 * holds the permission. The screen asks for it before it allows other devices ([com.nbtapps.store.ui.ssh]).
 */
fun localNetworkNeeded(sdk: Int, targetSdk: Int, granted: Boolean): Boolean = sdk >= ANDROID_17 && targetSdk >= ANDROID_17 && !granted

/**
 * Why other devices cannot reach the port, where the permission is still needed ([localNetworkNeeded]): the port's last
 * check, behind the screen's ask. Null where nothing stands in the way.
 */
fun localNetworkBlocked(sdk: Int, targetSdk: Int, granted: Boolean): String? =
    LOCAL_NETWORK_BLOCKED.takeIf { localNetworkNeeded(sdk, targetSdk, granted) }

private const val ANDROID_17 = 37

/** Android 17's local network permission, which the port's screen asks for. */
const val ACCESS_LOCAL_NETWORK = "android.permission.ACCESS_LOCAL_NETWORK"

/** What the port's screen says where Android keeps other devices out. */
const val LOCAL_NETWORK_BLOCKED =
    "Android keeps other devices from reaching this app until the Nearby devices permission is allowed."

/** The fingerprint of the ed25519 key among [hostKeys], or of the first where there is none. */
internal fun hostKeyFingerprint(hostKeys: List<String>): String? {
    val line = hostKeys.firstOrNull { SshKeyLines.type(it) == SshKeyLines.ED25519 } ?: hostKeys.firstOrNull() ?: return null
    return SshKeyLines.blob(line)?.let(SshKeyLines::fingerprint)
}

/**
 * The port on the phone for other SSH clients: an SSH app on the phone, or with other devices allowed a laptop on the
 * phone's hotspot or Wi-Fi. It lives as long as the process, as [com.nbtapps.store.data.terminal.TerminalRepository]
 * does, and is for one head unit at a time.
 *
 * [switchOn] needs a link to the head unit whose agent has channels, the SSH package with SSH access on, and at least
 * one client key saved ([AuthorizedClients]). It opens one tunnel as a check, which gives the head unit's host key,
 * closes it, and listens on 127.0.0.1 at [FIRST_PORT] or the next free port up to [LAST_PORT]. From then until it is
 * off, the port is a use of the link ([JobRepository.usingSession]), so the link is not dropped when the app is left,
 * and the foreground service runs for it ([SshPortState.active]).
 *
 * Each connection is a tunnel of its own ([SshTunnel]) with the saved client keys as they are when it is made, never
 * the app's own; the bytes go both ways as they are, and the client speaks SSH itself, checks the head unit's host key
 * and logs in with its own key. At most [MAX_CONNECTIONS] at once, one fewer than the head unit's relay takes, so that
 * the app's terminal has one; another is closed at once, as is one that comes while a job runs (the agent refuses the
 * open with `busy`, and holds the open ones meanwhile).
 *
 * It goes off by itself ([PortOffReason]) after [IDLE_MS] with no connection open, counted from when it went on and
 * from when the last connection closed, on a clock that counts sleep; when the link is lost (nothing connects again);
 * when SSH access is switched off on the head unit; and when the SSH package is removed or upgraded. Removing a saved
 * key closes every connection, and the port goes off with the last key. Going off closes every connection and the
 * socket.
 *
 * With other devices allowed ([allowOtherDevices]) it listens on the wildcard address, which on Android is every
 * address of the phone, IPv4 and IPv6, on Wi-Fi, the hotspot and mobile data alike, wherever the network lets a
 * connection in; Android's local network permission says nothing of the mobile network. It looks at nobody's address:
 * only the saved keys let a client log in, and the short time it is on limits the rest. That goes back to off each time
 * the port goes off.
 */
class SshPort(
    private val link: PortLink,
    private val channel: PortChannel,
    private val clients: AuthorizedClients,
    private val keeper: JobKeeper,
    private val scope: CoroutineScope,
    /** A clock in milliseconds that only goes forward and goes on while the phone sleeps: what the idle limit counts on. */
    private val clock: () -> Long = ::monotonicMillis,
    private val sockets: PortSockets = PlainPortSockets,
    private val networkAddresses: () -> List<PortAddress> = ::phoneAddresses,
    private val firstPort: Int = FIRST_PORT,
    private val lastPort: Int = LAST_PORT,
    private val idleMs: Long = IDLE_MS,
    private val idleCheckMs: Long = IDLE_CHECK_MS,
    private val pollMs: Long = POLL_MS,
    /**
     * Why other devices cannot reach the port on this phone, where they cannot; null where they can. Android 17 drops
     * connections from the local network to an app that targets it and has no local network permission, without
     * refusing the socket or telling the app: [localNetworkBlocked] in the app. The screen asks for the permission
     * first; this is the last check, for a permission revoked meanwhile.
     */
    private val otherDevicesBlocked: () -> String? = { null },
) {
    private val _state = MutableStateFlow<SshPortState>(SshPortState.Off())

    /** How the port stands. */
    val state: StateFlow<SshPortState> = _state.asStateFlow()

    /** Guards [running] and each [Run]'s fields that say so. */
    private val lock = Any()

    private var running: Run? = null

    /** One connection of a client: its socket, and the tunnel once it is open. */
    private class Connection(val socket: Socket) {
        @Volatile
        var tunnel: PortTunnel? = null

        @Volatile
        var paused = false

        fun close() {
            runCatching { socket.close() }
            tunnel?.close()
        }
    }

    /** The port from [switchOn] to off. */
    private class Run(val headUnitId: String) {
        lateinit var job: Job

        /** Completes once the port is to go off, with why: null for the user's own switching off. */
        val ending = CompletableDeferred<PortOffReason?>()

        /** Guarded by [lock]. */
        var server: ServerSocket? = null
        var port = 0
        var otherDevices = false
        var otherDevicesFailure: String? = null

        /** Why it went off where it lost its socket: shown as the off state's failure. */
        @Volatile
        var lost: String? = null
        var hostKey = ""
        val connections = LinkedHashSet<Connection>()

        /** A job runs on the head unit. */
        @Volatile
        var jobRuns = false

        /** When the last connection closed, or the port went on, on the clock. */
        @Volatile
        var since = 0L

        fun end(reason: PortOffReason?) {
            ending.complete(reason)
        }
    }

    /**
     * Switches the port on for the head unit [headUnitId]: see the class. How it goes is in [state]. False, and nothing
     * done, where it is on, or being switched on, for another head unit; true where it is on for this one already.
     */
    fun switchOn(headUnitId: String): Boolean {
        synchronized(lock) {
            running?.let { return it.headUnitId == headUnitId }
            val run = Run(headUnitId)
            running = run
            _state.value = SshPortState.Checking(headUnitId)
            run.job = scope.launch(start = CoroutineStart.ATOMIC) { lifetime(run) }
        }
        // As the user switches it on, with the app in front of the user: the service may be started.
        keeper.jobRunning()
        return true
    }

    /** Switches the port off, closing its connections and its socket: the user's, so no reason is kept. */
    fun switchOff() {
        val run = synchronized(lock) { running } ?: return
        run.end(null)
    }

    /**
     * SSH access on the head unit [headUnitId] was switched [on] or off from this app: off turns the port off at once,
     * where it would otherwise be noticed at the next look.
     */
    fun noteSwitch(headUnitId: String, on: Boolean) {
        if (on) return
        synchronized(lock) { running?.takeIf { it.headUnitId == headUnitId } }?.end(PortOffReason.SWITCHED_OFF)
    }

    /**
     * Lets other devices reach the port, or no longer: it listens on every address of the phone, or on 127.0.0.1 again,
     * on the same port. Connections that are open stay. Only while it is on; it goes back to off when the port does.
     */
    fun allowOtherDevices(allow: Boolean) {
        val run = synchronized(lock) { running?.takeIf { _state.value is SshPortState.On && it.otherDevices != allow } } ?: return
        if (allow) {
            otherDevicesBlocked()?.let { why ->
                // Android would take the socket and drop every connection from another device unseen.
                synchronized(lock) { run.otherDevicesFailure = why }
                publish(run)
                return
            }
        }
        scope.launch(Dispatchers.IO) {
            synchronized(lock) {
                if (running !== run || run.ending.isCompleted || run.otherDevices == allow) return@launch
                val old = run.server
                // The port is let go first: a socket on every address and one on 127.0.0.1 cannot share it.
                runCatching { old?.close() }
                run.server = try {
                    listenAgain(allow, run.port).also {
                        run.otherDevices = allow
                        run.otherDevicesFailure = null
                    }
                } catch (e: IOException) {
                    run.otherDevicesFailure = "Other devices couldn't be let in: ${e.message ?: e.javaClass.simpleName}"
                    try {
                        listenAgain(run.otherDevices, run.port)
                    } catch (again: IOException) {
                        // The port is gone from under it: it goes off, and says so.
                        run.lost = "The port couldn't be opened again on this phone: ${again.message ?: again.javaClass.simpleName}"
                        run.end(null)
                        null
                    }
                }
            }
            publish(run)
        }
    }

    /**
     * The port taken again just after its socket was closed. The thread waiting in accept() on the old socket holds it
     * for a moment after the close, so a port still in use is tried a few times over a short while.
     */
    private fun listenAgain(allDevices: Boolean, port: Int): ServerSocket {
        var tries = 0
        while (true) {
            try {
                return sockets.listen(allDevices, port)
            } catch (e: IOException) {
                if (++tries >= REBIND_TRIES) throw e
                Thread.sleep(REBIND_WAIT_MS)
            }
        }
    }

    /** What [switchOn] began: the port from its check to off, and the state it ends in. */
    private suspend fun lifetime(run: Run) {
        var end: SshPortState = SshPortState.Off(run.headUnitId)
        try {
            end = attempt(run)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Nothing that goes wrong here may end the app.
            end = SshPortState.Off(run.headUnitId, failure = "The port ran into a fault: ${e.message ?: e.javaClass.simpleName}")
        } finally {
            finish(run, end)
        }
    }

    private suspend fun attempt(run: Run): SshPortState {
        val id = run.headUnitId
        val keys = clients.clients.first().map { it.line }
        if (keys.isEmpty()) return SshPortState.Off(id, failure = NO_KEYS)
        val connected = (link.connection.value as? ConnectionState.Connected)?.takeIf { it.headUnitId == id }
            ?: return SshPortState.Off(id, failure = NOT_CONNECTED)
        if (!connected.channels) return SshPortState.Off(id, failure = TOO_OLD)
        val version = connected.installed.find { it.id == SshTunnel.PACKAGE }?.version ?: return SshPortState.Off(id, failure = NO_PACKAGE)
        return link.usingSession(id) { session -> over(run, session, keys, version) }
            .getOrElse { SshPortState.Off(id, failure = it.message ?: NOT_CONNECTED) }
    }

    /** The check, the socket, and the port served until it is to go off, as a use of the link. */
    private suspend fun over(run: Run, session: AgentSession, keys: List<String>, version: String): SshPortState {
        val id = run.headUnitId
        val hostKey = try {
            val tunnel = channel.open(session, keys)
            try {
                hostKeyFingerprint(tunnel.hostKeys)
            } finally {
                withContext(NonCancellable + Dispatchers.IO) { tunnel.close() }
            }
        } catch (e: SshTunnelException) {
            return SshPortState.Off(id, failure = failureLine(e.failure))
        } catch (e: IOException) {
            return SshPortState.Off(id, failure = e.message ?: NOT_CONNECTED)
        } ?: return SshPortState.Off(id, failure = "The head unit's SSH server gave no host key this app can read.")
        // Switched off while it checked.
        if (run.ending.isCompleted) return SshPortState.Off(id, reason = run.ending.await())

        val server = withContext(Dispatchers.IO) { listen() } ?: return SshPortState.Off(id, failure = NO_FREE_PORT)
        synchronized(lock) {
            run.server = server.first
            run.port = server.second
            run.hostKey = hostKey
            run.since = clock()
        }
        publish(run)

        val reason = coroutineScope {
            val helpers = listOf(
                launch(Dispatchers.IO) { accept(run, session) },
                launch { watchLink(run, session) },
                launch { watchPackage(run, version) },
                launch { pollSwitch(run, session) },
                launch { idle(run) },
                launch { watchKeys(run) },
                launch { watchJob(run) },
            )
            val why = run.ending.await()
            synchronized(lock) {
                runCatching { run.server?.close() }
                run.server = null
                run.connections.toList()
            }.forEach { withContext(Dispatchers.IO) { it.close() } }
            helpers.forEach { it.cancel() }
            why
        }
        return SshPortState.Off(id, reason = reason, failure = run.lost)
    }

    /** The first port from [firstPort] to [lastPort] that can be had on 127.0.0.1, and its socket. */
    private fun listen(): Pair<ServerSocket, Int>? {
        for (port in firstPort..lastPort) {
            try {
                return sockets.listen(false, port) to port
            } catch (e: IOException) {
                // Taken: the next.
            }
        }
        return null
    }

    /** Takes connections until the port is to go off, each served in [scope] of its own. */
    private suspend fun accept(run: Run, session: AgentSession) = coroutineScope {
        while (!run.ending.isCompleted) {
            val server = synchronized(lock) { run.server } ?: break
            val socket = try {
                server.accept()
            } catch (e: IOException) {
                // Closed: the port goes off, or listens on another socket now.
                if (run.ending.isCompleted) break
                val now = synchronized(lock) { run.server }
                if (now === server && server.isClosed) break
                continue
            }
            val connection = Connection(socket)
            val taken = synchronized(lock) {
                if (run.connections.size >= MAX_CONNECTIONS || run.ending.isCompleted) {
                    false
                } else {
                    run.connections += connection
                    true
                }
            }
            if (!taken) {
                runCatching { socket.close() }
                continue
            }
            publish(run)
            launch { serve(run, session, connection) }
        }
    }

    /** One connection: its tunnel, and the bytes both ways until either side ends. */
    private suspend fun serve(run: Run, session: AgentSession, connection: Connection) = coroutineScope {
        try {
            val keys = clients.clients.first().map { it.line }
            if (keys.isEmpty()) return@coroutineScope
            val tunnel = try {
                channel.open(session, keys)
            } catch (e: SshTunnelException) {
                when ((e.failure as? TunnelFailure.Refused)?.code) {
                    ChannelError.OFF -> run.end(PortOffReason.SWITCHED_OFF)
                    ChannelError.NOT_FOUND -> run.end(PortOffReason.PACKAGE_CHANGED)
                }
                return@coroutineScope
            } catch (e: IOException) {
                return@coroutineScope
            }
            connection.tunnel = tunnel
            // Closed while it opened: the tunnel goes too.
            if (synchronized(lock) { connection !in run.connections }) return@coroutineScope
            val watch = launch { follow(run, connection, tunnel) }
            listOf(
                launch(Dispatchers.IO) { pipe(connection.socket.getInputStream(), tunnel.output, connection) },
                launch(Dispatchers.IO) { pipe(tunnel.input, connection.socket.getOutputStream(), connection) },
            ).joinAll()
            watch.cancel()
        } finally {
            withContext(NonCancellable + Dispatchers.IO) { connection.close() }
            synchronized(lock) {
                run.connections.remove(connection)
                if (run.connections.isEmpty()) run.since = clock()
            }
            publish(run)
        }
    }

    /** Copies [from] to [to] until either ends, then closes both sides of [connection]. */
    private fun pipe(from: InputStream, to: OutputStream, connection: Connection) {
        try {
            val buffer = ByteArray(PIPE_BUFFER)
            while (true) {
                val n = from.read(buffer)
                if (n < 0) break
                to.write(buffer, 0, n)
                to.flush()
            }
        } catch (e: IOException) {
            // Either side gone.
        } finally {
            connection.close()
        }
    }

    /** Follows a connection's channel: held during a job, and an end that tells the port to go off. */
    private suspend fun follow(run: Run, connection: Connection, tunnel: PortTunnel) {
        tunnel.state.collect { now ->
            connection.paused = now == ChannelState.Paused
            publish(run)
            val end = (now as? ChannelState.Closed)?.end
            when {
                end is ChannelEnd.ByAgent && end.reason == ChannelCloseReason.OFF -> run.end(PortOffReason.SWITCHED_OFF)
                end is ChannelEnd.ByAgent && end.reason == ChannelCloseReason.PACKAGE_CHANGED -> run.end(PortOffReason.PACKAGE_CHANGED)
                end is ChannelEnd.LinkLost -> run.end(PortOffReason.LINK_LOST)
            }
        }
    }

    /** The link to the head unit ended, or is no longer to it. */
    private suspend fun watchLink(run: Run, session: AgentSession) = coroutineScope {
        val ended = launch {
            session.ended.await()
            run.end(PortOffReason.LINK_LOST)
        }
        link.connection.first { (it as? ConnectionState.Connected)?.headUnitId != run.headUnitId }
        run.end(PortOffReason.LINK_LOST)
        ended.cancel()
    }

    /** The SSH package was removed or upgraded, as the agent's list of packages says. */
    private suspend fun watchPackage(run: Run, version: String) {
        link.connection.first { now ->
            val connected = (now as? ConnectionState.Connected)?.takeIf { it.headUnitId == run.headUnitId } ?: return@first false
            connected.installed.find { it.id == SshTunnel.PACKAGE }?.version != version
        }
        run.end(PortOffReason.PACKAGE_CHANGED)
    }

    /**
     * Looks at SSH access every [pollMs]: switched off, or the package gone, from elsewhere; and with other devices
     * allowed, at the phone's addresses.
     */
    private suspend fun pollSwitch(run: Run, session: AgentSession) {
        while (true) {
            delay(pollMs)
            when (channel.switch(session)) {
                SwitchReading.OFF -> run.end(PortOffReason.SWITCHED_OFF)
                SwitchReading.GONE -> run.end(PortOffReason.PACKAGE_CHANGED)
                else -> {}
            }
            // The phone's addresses are read again with it, so that a hotspot switched on since shows: the state is
            // only given out anew where they changed.
            if (synchronized(lock) { run.otherDevices }) withContext(Dispatchers.IO) { publish(run) }
        }
    }

    /** Goes off once there has been no connection for [idleMs] on the clock, looked at every [idleCheckMs] at most. */
    private suspend fun idle(run: Run) {
        while (true) {
            val (open, since) = synchronized(lock) { run.connections.isNotEmpty() to run.since }
            if (open) {
                delay(idleCheckMs)
                continue
            }
            val left = since + idleMs - clock()
            if (left <= 0) break
            delay(minOf(left, idleCheckMs))
        }
        run.end(PortOffReason.IDLE)
    }

    /** A saved key removed: every connection is closed; with none left, the port goes off. */
    private suspend fun watchKeys(run: Run) {
        var known: Set<String>? = null
        clients.clients.collect { now ->
            val prints = now.map { it.fingerprint }.toSet()
            val before = known
            known = prints
            if (prints.isEmpty()) run.end(null)
            if (before != null && !prints.containsAll(before)) {
                synchronized(lock) { run.connections.toList() }.forEach { withContext(Dispatchers.IO) { it.close() } }
            }
        }
    }

    /** Whether a job runs on the head unit, which holds the connections: for [SshPortState.On.paused]. */
    private suspend fun watchJob(run: Run) {
        link.jobOn.collect { on ->
            run.jobRuns = on == run.headUnitId
            publish(run)
        }
    }

    /** Shows [run] as it is now, while it is the one that runs and is not over. */
    private fun publish(run: Run) {
        synchronized(lock) {
            if (running !== run || run.server == null && _state.value !is SshPortState.On) return
            if (run.ending.isCompleted) return
            _state.value = SshPortState.On(
                headUnitId = run.headUnitId,
                port = run.port,
                otherDevices = run.otherDevices,
                connections = run.connections.size,
                paused = run.jobRuns || run.connections.any { it.paused },
                addresses = listOf(PortAddress(PortAddress.Kind.THIS_PHONE, LOOPBACK)) + if (run.otherDevices) networkAddresses() else emptyList(),
                hostKey = run.hostKey,
                otherDevicesFailure = run.otherDevicesFailure,
            )
        }
    }

    /** The port is off, in [end]: its socket and connections closed, and the next [switchOn] may begin. */
    private fun finish(run: Run, end: SshPortState) {
        val left = synchronized(lock) {
            runCatching { run.server?.close() }
            run.server = null
            run.ending.complete(null)
            _state.value = end
            running = null
            run.connections.toList()
        }
        left.forEach { it.close() }
    }

    /** What a tunnel that could not be had is shown as, where the port could not be switched on. */
    private fun failureLine(failure: TunnelFailure): String = when (failure) {
        is TunnelFailure.Refused -> when (failure.code) {
            ChannelError.OFF -> "SSH access is switched off on the head unit."
            ChannelError.NOT_FOUND -> NO_PACKAGE
            ChannelError.BUSY -> "The head unit is busy with a job. Try again once it has finished."
            ChannelError.UNSUPPORTED -> TOO_OLD
            ChannelError.UNAVAILABLE -> "The SSH package on the head unit didn't start."
            ChannelError.TOO_MANY -> "The link to the head unit has as many connections open as it can take."
            else -> "The head unit refused SSH (${failure.code})."
        }
        is TunnelFailure.Relay -> when (failure.code) {
            RelayError.TOO_MANY -> "The head unit's SSH server has as many connections as it takes."
            RelayError.BAD_KEYS -> "The SSH package on the head unit didn't take the saved keys."
            RelayError.FAILED -> "The head unit's SSH server couldn't be started."
            else -> "The SSH package on the head unit refused the connection (${failure.code})."
        }
        is TunnelFailure.BadAnswer -> "The SSH package on the head unit sent an answer this app can't read."
        is TunnelFailure.Closed -> when (val end = failure.end) {
            is ChannelEnd.ByAgent -> if (end.reason == ChannelCloseReason.OFF) "SSH access is switched off on the head unit." else "The head unit closed the SSH connection before it was up."
            is ChannelEnd.LinkLost -> end.reason
            else -> "The SSH connection was closed before it was up."
        }
        is TunnelFailure.NoAnswer -> "The head unit didn't answer in time."
    }

    companion object {
        /** The first port tried, and the last. */
        const val FIRST_PORT = 2222
        const val LAST_PORT = 2231

        /** The address every port listens on. */
        const val LOOPBACK = "127.0.0.1"

        /** Connections at once: one fewer than the head unit's relay serves, which leaves one for the app's terminal. */
        const val MAX_CONNECTIONS = 3

        /** The port goes off after this long with no connection open, in milliseconds. */
        const val IDLE_MS = 30 * 60_000L

        /** How often the idle limit is looked at, at most: a wait does not count the time the phone slept. */
        const val IDLE_CHECK_MS = 60_000L

        /** How often SSH access is looked at on the head unit while the port is on. */
        const val POLL_MS = 15_000L

        private const val PIPE_BUFFER = 8192

        /** How often, and how far apart, the port is tried again after its socket was closed for another: [listenAgain]. */
        private const val REBIND_TRIES = 10
        private const val REBIND_WAIT_MS = 50L

        const val NO_KEYS = "Add a key first."
        const val NOT_CONNECTED = "Connect to the head unit first."
        const val NO_PACKAGE = "SSH access isn't installed on this head unit."
        const val TOO_OLD = "This head unit's agent is too old for SSH access. Update the agent."
        const val NO_FREE_PORT = "No port from 2222 to 2231 is free on this phone."
    }
}

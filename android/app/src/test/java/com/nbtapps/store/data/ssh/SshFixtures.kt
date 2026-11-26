// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.ChannelSwitch
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.TcpEndpoint
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map

/** What the tests of SSH in the app share: two head units, their links, the catalogue's offer, and a stand-in switch. */
internal object SshFixtures {
    const val CAR = "car"
    const val OTHER = "other"
    val NAMES = mapOf(CAR to "Test car", OTHER to "Other car")

    /** A link to [headUnitId], whose agent runs [agent], has channels where [channels], and the SSH package at [ssh]. */
    fun connected(headUnitId: String = CAR, ssh: String? = "1.0.0", channels: Boolean = true, agent: String = "1.7.0") = ConnectionState.Connected(
        headUnitId,
        TcpEndpoint("192.0.2.1", 7144),
        AgentInfo(agent, "000102030405060708090a0b0c0d0e0f", 1),
        listOfNotNull(ssh?.let { InstalledPackage(SshTunnel.PACKAGE, "SSH access", it) }),
        capabilities = if (channels) listOf(ProtocolConstants.CAPABILITY_CHANNELS) else emptyList(),
    )

    fun sshVersion(requires: String = ">=1.7.0-beta.1") = PackageVersion(
        version = "1.0.0",
        url = "https://example.invalid/ssh-1.0.0.nbtpkg",
        size = 1,
        sha256 = "00",
        manifestSha256 = "11",
        requires = PackageRequirements(agent = requires, app = ">=0.9.0"),
    )

    /** A catalogue that has the SSH package at [versions]; none of it with no versions. */
    fun offering(vararg versions: PackageVersion) = CatalogueState(
        CatalogueIndex(
            schema = 1,
            source = CatalogueSourceInfo("test", "Test"),
            serial = 1,
            packages = if (versions.isEmpty()) emptyList() else listOf(CataloguePackage(id = SshTunnel.PACKAGE, name = "SSH access", versions = versions.toList())),
        ),
    )
}

/**
 * The agent's `channel.get` and `channel.set` for the SSH package's channel, as [SshAccess] asks them: each request is
 * kept, and answered at once from [on] and [open], or once its gate is opened where the test set one ([readGate],
 * [writeGate]), so that a test puts requests and answers in the order it means to. [readFailure] and [writeFailure]
 * answer with a failure in place of the switch. One thread runs it all, the test's.
 */
internal class FakeSwitch {
    var on = false
    var open = 0
    val reads = mutableListOf<String>()
    val writes = mutableListOf<Pair<String, Boolean>>()
    val notes = mutableListOf<Pair<String, Boolean>>()
    var readGate: CompletableDeferred<Unit>? = null
    var writeGate: CompletableDeferred<Unit>? = null
    var readFailure: Throwable? = null
    var writeFailure: Throwable? = null

    suspend fun read(headUnitId: String): Result<ChannelSwitch> {
        reads += headUnitId
        readGate?.await()
        readFailure?.let { return Result.failure(it) }
        return Result.success(ChannelSwitch(switched = true, on = on, open = open))
    }

    suspend fun write(headUnitId: String, on: Boolean): Result<ChannelSwitch> {
        writes += headUnitId to on
        writeGate?.await()
        writeFailure?.let { return Result.failure(it) }
        this.on = on
        return Result.success(ChannelSwitch(switched = true, on = on, open = open))
    }
}

/** What [SshAccess] follows, each a flow the test sets. */
internal class SshInputs {
    val connection = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    val jobOn = MutableStateFlow<String?>(null)
    val terminal = MutableStateFlow<TerminalState>(TerminalState.Idle)
    val port = MutableStateFlow<SshPortState>(SshPortState.Off())
    val catalogue = MutableStateFlow(CatalogueState())
    val switch = FakeSwitch()

    /** The app's SSH switch over these, in [scope]. */
    fun access(scope: CoroutineScope, names: Flow<Map<String, String>> = flowOf(SshFixtures.NAMES)) = SshAccess(
        connection,
        jobOn,
        terminal,
        port,
        switch::read,
        switch::write,
        { id, on -> switch.notes += id to on },
        scope,
        catalogue = catalogue,
        names = names,
    )
}

/** The app's SSH switch over the app's own link [jobs], as the app makes it but for the terminal and the port, which are [terminal] and [port]. */
internal fun sshAccessOver(
    jobs: JobRepository,
    scope: CoroutineScope,
    terminal: Flow<TerminalState> = flowOf(TerminalState.Idle),
    port: Flow<SshPortState> = flowOf(SshPortState.Off()),
    catalogue: Flow<CatalogueState> = flowOf(CatalogueState()),
    noted: (String, Boolean) -> Unit = { _, _ -> },
) = SshAccess(
    jobs.connection,
    jobs.job.map { view -> view?.takeIf { !it.ended }?.headUnitId },
    terminal,
    port,
    { id -> jobs.channelSwitch(id, SshTunnel.PACKAGE, SshTunnel.CHANNEL) },
    { id, on -> jobs.setChannelSwitch(id, SshTunnel.PACKAGE, SshTunnel.CHANNEL, on) },
    noted,
    scope,
    catalogue = catalogue,
)

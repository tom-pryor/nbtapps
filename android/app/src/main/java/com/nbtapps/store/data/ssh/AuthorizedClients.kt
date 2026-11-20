// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import androidx.datastore.core.CorruptionException
import androidx.datastore.core.DataStore
import androidx.datastore.core.DataStoreFactory
import androidx.datastore.core.Serializer
import androidx.datastore.core.handlers.ReplaceFileCorruptionHandler
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.SerializationException
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json

/**
 * The public key of another SSH client, one that the user added so that it may log in to a head unit through the
 * phone: an SSH app on the phone, or a laptop on its hotspot. [line] passes the relay's rule ([SshKeyLines]).
 */
@Serializable
data class ClientKey(
    /** What the user called it. */
    val name: String,
    /** The key as one line of `authorized_keys`, as it goes in the keys message. */
    val line: String,
) {
    /** The key's fingerprint, as OpenSSH shows it: what tells one key from another, and what [AuthorizedClients.remove] takes. */
    val fingerprint: String get() = SshKeyLines.fingerprint(requireNotNull(SshKeyLines.blob(line)) { "not a key line: $line" })
}

/** What became of a key the user gave [AuthorizedClients.add]. */
sealed interface AddClientResult {
    data class Added(val client: ClientKey) : AddClientResult

    /** The line is not one the head unit takes, for [why] (in words for the user). */
    data class BadLine(val why: String) : AddClientResult

    /** The name is empty, or longer than [AuthorizedClients.MAX_NAME]. */
    data object BadName : AddClientResult

    /** As many keys as a connection may carry are kept already: [AuthorizedClients.MAX_CLIENTS]. */
    data object Full : AddClientResult

    /** The key is kept already, under [name]. */
    data class Already(val name: String) : AddClientResult
}

/**
 * The public keys of the other SSH clients the user added, each with a name: the keys that the app sends with its own
 * in the keys message of a connection for another client, and that may log in on it. They are the phone's, not a
 * head unit's: the same keys go to every head unit, and the head unit keeps none of them.
 *
 * A line is held to the relay's rule before it is kept ([SshKeyLines]): it is tidied first as a pasted line is
 * ([SshKeyLines.tidy]), and one that then does not pass is refused. At most [MAX_CLIENTS] are kept, so that they and
 * the app's own key fit one keys message.
 */
interface AuthorizedClients {
    /** The keys in the order they were added. */
    val clients: Flow<List<ClientKey>>

    /** Keeps the key of [pasted] under [name], or says why it does not. */
    suspend fun add(name: String, pasted: String): AddClientResult

    /** Removes the key of [fingerprint]; nothing happens if there is none. */
    suspend fun remove(fingerprint: String)

    companion object {
        /** The keys of other clients that fit one keys message beside the app's own. */
        const val MAX_CLIENTS = SshKeyLines.MAX_KEYS - 1

        /** The longest name, in characters. */
        const val MAX_NAME = 64

        /** What [add] makes of the request against the keys kept now: the new list, or why there is none. */
        internal fun adding(now: List<ClientKey>, name: String, pasted: String): Pair<List<ClientKey>?, AddClientResult> {
            val named = name.trim()
            if (named.isEmpty() || named.length > MAX_NAME) return null to AddClientResult.BadName
            val line = SshKeyLines.tidy(pasted)
            SshKeyLines.problem(line)?.let { return null to AddClientResult.BadLine(it) }
            val client = ClientKey(named, line)
            now.firstOrNull { it.fingerprint == client.fingerprint }?.let { return null to AddClientResult.Already(it.name) }
            if (now.size >= MAX_CLIENTS) return null to AddClientResult.Full
            return now + client to AddClientResult.Added(client)
        }

        /** What a stored list gives: the keys that pass this version's rule, the first of each key, at most [MAX_CLIENTS]. */
        internal fun usable(stored: List<ClientKey>): List<ClientKey> =
            stored.filter { SshKeyLines.passes(it.line) && it.name.isNotBlank() }.distinctBy { it.fingerprint }.take(MAX_CLIENTS)
    }
}

/** Keeps the keys in memory, for tests and previews. */
class InMemoryAuthorizedClients : AuthorizedClients {
    private val keys = MutableStateFlow<List<ClientKey>>(emptyList())

    override val clients: Flow<List<ClientKey>> = keys

    override suspend fun add(name: String, pasted: String): AddClientResult = synchronized(keys) {
        val (next, result) = AuthorizedClients.adding(keys.value, name, pasted)
        if (next != null) keys.value = next
        result
    }

    override suspend fun remove(fingerprint: String) = synchronized(keys) { keys.value = keys.value.filter { it.fingerprint != fingerprint } }
}

/** Keeps the keys in a DataStore, so they are still there the next time the app starts. */
class DataStoreAuthorizedClients(private val store: DataStore<List<ClientKey>>) : AuthorizedClients {

    // Keys that cannot be read are none: another client then cannot log in until they are added again, which is the
    // side to fail on.
    override val clients: Flow<List<ClientKey>> =
        store.data.map(AuthorizedClients::usable).catch { e -> if (e is IOException) emit(emptyList()) else throw e }

    override suspend fun add(name: String, pasted: String): AddClientResult {
        var result: AddClientResult = AddClientResult.Full
        store.updateData { stored ->
            val (next, outcome) = AuthorizedClients.adding(AuthorizedClients.usable(stored), name, pasted)
            result = outcome
            next ?: stored
        }
        return result
    }

    override suspend fun remove(fingerprint: String) {
        store.updateData { stored -> stored.filter { !SshKeyLines.passes(it.line) || it.fingerprint != fingerprint } }
    }
}

/** The keys as one JSON array of `{"name", "line"}`, in order, which is how the DataStore file holds them. */
object ClientKeysSerializer : Serializer<List<ClientKey>> {
    private val json = Json { ignoreUnknownKeys = true }
    private val serializer = ListSerializer(ClientKey.serializer())

    override val defaultValue: List<ClientKey> = emptyList()

    override suspend fun readFrom(input: InputStream): List<ClientKey> = try {
        json.decodeFromString(serializer, input.readBytes().decodeToString())
    } catch (e: SerializationException) {
        throw CorruptionException("The SSH clients' keys file is not one this version can read", e)
    } catch (e: IllegalArgumentException) {
        throw CorruptionException("The SSH clients' keys file is not one this version can read", e)
    }

    override suspend fun writeTo(t: List<ClientKey>, output: OutputStream) {
        output.write(json.encodeToString(serializer, t).encodeToByteArray())
    }
}

/**
 * The DataStore that keeps the other clients' keys in [file]. Only one may be open on a file at a time; it is closed
 * when [scope] ends.
 */
fun authorizedClientsStore(file: File, scope: CoroutineScope): DataStore<List<ClientKey>> = DataStoreFactory.create(
    serializer = ClientKeysSerializer,
    // A file this version cannot read is left as no keys: they are public keys, which the user still has.
    corruptionHandler = ReplaceFileCorruptionHandler { emptyList() },
    scope = scope,
    produceFile = { file },
)

/** The lines of the keys message for a connection of another client: the app's own key first, then the clients'. */
suspend fun AuthorizedClients.keysWith(identity: SshIdentity): List<String> = listOf(identity.publicKeyLine()) + clients.first().map { it.line }

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.link.Blob
import com.nbtapps.store.model.Offer
import com.nbtapps.store.model.PackageDependency
import com.nbtapps.store.model.PackageVersion
import java.io.File
import java.io.IOException
import java.io.InputStream
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.Json

/** A verified archive the phone holds, with what the app needs to know about it without opening it again. */
@Serializable
data class StoredPackage(
    val id: String,
    val name: String,
    val version: String,
    /** SHA-256 of the archive file, in hex. Also the name it is stored under. */
    val sha256: String,
    val size: Long,
    val manifestSha256: String,
    /**
     * The signature that verified the manifest, in hex: the catalogue's detached one where [detachedSignature],
     * otherwise the contents of `manifest.sig`. It is the one the agent is offered, and checks again.
     */
    val signature: String,
    /** The public key that verified the signature, in hex. */
    val key: String,
    /**
     * [signature] is the catalogue's own, over a package its author signed: only an agent that lists
     * [CAPABILITY_DETACHED_SIGNATURE][com.nbtapps.store.data.link.ProtocolConstants.CAPABILITY_DETACHED_SIGNATURE]
     * installs it.
     */
    val detachedSignature: Boolean = false,
    /** The risk flags, worked out from the manifest. */
    val flags: List<String> = emptyList(),
    val depends: List<PackageDependency> = emptyList(),
    /** The manifest's `requires.agent` range, if it has one. */
    val requiresAgent: String? = null,
) {
    /** What the agent is told about this archive in `job.plan`. */
    fun offer() = Offer(id, version, sha256, size, manifestSha256, signature)
}

/**
 * The downloaded archives, kept in the app's files (not its cache: they must survive until the phone is in the
 * car). An archive gets in only after [PackageArchive.verify] has read every byte of it, and is stored under
 * its own hash:
 *
 *     <sha256>.nbtpkg     the archive, as downloaded
 *     <sha256>.manifest   the exact bytes of its manifest.json, which the agent fetches as a blob of its own
 *     <sha256>.json       a [StoredPackage]; written last, so its presence says the other two are whole
 */
class PackageStore(
    private val dir: File,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    private val stored = MutableStateFlow<List<StoredPackage>>(emptyList())

    /** Every archive held, sorted by id and then by version text. Empty until [load] has run. */
    val packages: StateFlow<List<StoredPackage>> = stored.asStateFlow()

    private val loaded = CompletableDeferred<Unit>()

    /** Waits until [load] has run, so an empty [packages] means that nothing is stored. */
    suspend fun awaitLoaded() = loaded.await()

    /** Reads what is on disk, and clears away downloads that were cut off. */
    suspend fun load() = withContext(io) {
        dir.mkdirs()
        val found = ArrayList<StoredPackage>()
        for (file in dir.listFiles().orEmpty()) {
            when (file.extension) {
                "part" -> file.delete()
                "json" -> {
                    val pkg = try {
                        json.decodeFromString(StoredPackage.serializer(), file.readText())
                    } catch (e: SerializationException) {
                        null
                    } catch (e: IOException) {
                        null
                    }
                    if (pkg != null && file.nameWithoutExtension == pkg.sha256 && archive(pkg).length() == pkg.size && manifest(pkg).isFile) {
                        found += pkg
                    }
                }
            }
        }
        stored.value = found.sortedWith(ORDER)
        loaded.complete(Unit)
    }

    fun find(id: String, version: String): StoredPackage? = stored.value.find { it.id == id && it.version == version }

    /** The archive file of [pkg]. */
    fun archive(pkg: StoredPackage): File = File(dir, "${pkg.sha256}.nbtpkg")

    private fun manifest(pkg: StoredPackage): File = File(dir, "${pkg.sha256}.manifest")

    /**
     * The two blobs the agent may fetch for [pkg], by the hashes it asks with: the archive and its manifest.
     *
     * @throws IOException if the stored manifest cannot be read.
     */
    suspend fun blobs(pkg: StoredPackage): Map<String, Blob> = withContext(io) {
        mapOf(
            pkg.sha256 to Blob.OfFile(archive(pkg), pkg.size),
            pkg.manifestSha256 to Blob.OfBytes(manifest(pkg).readBytes()),
        )
    }

    /**
     * Copies an archive from [input] into the store, then verifies it: the container profile, the signature
     * against [trustedKeys], every file against the manifest and, with [expected], the whole against the
     * catalogue entry that led to it. Nothing is kept unless all of it holds. [onProgress] gets the bytes
     * copied so far.
     *
     * The signature is the archive's `manifest.sig`, or where the entry [expected] has one, the entry's
     * [signature][PackageVersion.signature] in its place.
     *
     * @throws PackageFormatException if the archive is refused. The message is for the user.
     * @throws IOException if it cannot be read or stored.
     */
    suspend fun add(
        input: InputStream,
        trustedKeys: List<ByteArray>,
        expected: Pair<String, PackageVersion>? = null,
        onProgress: (Long) -> Unit = {},
    ): StoredPackage = withContext(io) {
        dir.mkdirs()
        val part = File.createTempFile("download-", ".part", dir)
        try {
            // An archive is below 4 GiB, and one the catalogue lists has exactly the size the catalogue says.
            val limit = expected?.second?.size ?: MAX_ARCHIVE_BYTES
            var copied = 0L
            part.outputStream().use { out ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    copied += count
                    if (copied > limit) throw PackageFormatException("The package is larger than the catalogue says it is")
                    out.write(buffer, 0, count)
                    onProgress(copied)
                }
            }
            val detached = expected?.second?.signature?.let { hex ->
                Hex.decodeOrNull(hex, Ed25519.SIGNATURE_BYTES)
                    ?: throw PackageFormatException("The catalogue's signature for the package isn't valid", PackageFormatException.BAD_SIGNATURE)
            }
            val verified = PackageArchive.verify(part, trustedKeys, detached)
            expected?.let { (id, version) -> verified.requireMatches(id, version) }

            val pkg = StoredPackage(
                id = verified.manifest.id,
                name = verified.manifest.name,
                version = verified.manifest.version,
                sha256 = verified.sha256,
                size = verified.size,
                manifestSha256 = verified.manifestSha256,
                signature = Hex.encode(verified.signature),
                key = Hex.encode(verified.key),
                detachedSignature = detached != null,
                flags = verified.manifest.flags,
                depends = verified.manifest.depends,
                requiresAgent = verified.manifest.requires.agent,
            )
            manifest(pkg).writeBytes(verified.manifestBytes)
            if (!part.renameTo(archive(pkg))) throw IOException("Couldn't store the package")
            File(dir, "${pkg.sha256}.json").writeText(json.encodeToString(StoredPackage.serializer(), pkg))
            stored.update { list -> (list.filterNot { it.sha256 == pkg.sha256 } + pkg).sortedWith(ORDER) }
            pkg
        } finally {
            part.delete()
        }
    }

    /** Removes [pkg] from the phone. The head unit is not touched. */
    suspend fun delete(pkg: StoredPackage) = withContext(io) {
        // The record goes first, so a delete that is cut off leaves nothing that looks whole.
        File(dir, "${pkg.sha256}.json").delete()
        archive(pkg).delete()
        manifest(pkg).delete()
        stored.update { list -> list.filterNot { it.sha256 == pkg.sha256 } }
    }

    private companion object {
        const val MAX_ARCHIVE_BYTES = 0xFFFFFFFFL
        val json = Json { ignoreUnknownKeys = true }
        val ORDER = compareBy<StoredPackage>({ it.id }, { it.version })
    }
}

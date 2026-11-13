// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.packages.ManifestParser
import com.nbtapps.store.model.AppRelease
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.SemVer
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.decodeFromJsonElement

/** The catalogue file could be read but not used. The message is shown to the user. */
class CatalogueFormatException(message: String, cause: Throwable? = null) : Exception(message, cause)

/** Turns the published catalogue files into models. Unknown fields are ignored, as the plan requires. */
object CatalogueParser {
    const val SUPPORTED_SCHEMA = 1

    private val json = Json { ignoreUnknownKeys = true }

    /** The largest APK the app will download as its own update. The catalogue's own limit is far below it. */
    const val MAX_APP_BYTES = 256L * 1024 * 1024

    /**
     * Parses `index.json`. Versions that are not valid semver are dropped, along with packages left
     * with no version, and each package's versions come back newest first.
     *
     * The app's own release (`app`) is read apart from the rest: one that is malformed or cannot be used is
     * left out, and the packages are listed all the same. A catalogue is not refused for it.
     *
     * So are the packages of outside developers (`community`), each on its own, and they join the source's own
     * in [CatalogueIndex.packages] marked [CataloguePackage.community]. Left out are a community package that
     * is malformed, has an id reserved for the source's own (`org.nbtapp.`) or one that is listed already, and
     * a community version without the source's signature ([PackageVersion.signature]). The source's own
     * versions never carry one: a `signature` there is dropped, and the archive's `manifest.sig` is checked.
     */
    fun parseIndex(bytes: ByteArray): CatalogueIndex {
        val tree = try {
            json.parseToJsonElement(bytes.decodeToString())
        } catch (e: SerializationException) {
            throw CatalogueFormatException("The catalogue index is malformed", e)
        }
        if (tree !is JsonObject) throw CatalogueFormatException("The catalogue index is malformed")
        val index = try {
            json.decodeFromJsonElement<CatalogueIndex>(JsonObject(tree - APP - COMMUNITY))
        } catch (e: SerializationException) {
            throw CatalogueFormatException("The catalogue index is malformed", e)
        } catch (e: IllegalArgumentException) {
            throw CatalogueFormatException("The catalogue index is malformed", e)
        }
        if (index.schema != SUPPORTED_SCHEMA) {
            throw CatalogueFormatException(
                "Catalogue schema ${index.schema} isn't supported by this version of the app",
            )
        }
        val official = index.packages.mapNotNull { pkg ->
            sorted(pkg.copy(versions = pkg.versions.map { it.copy(signature = null) }))
        }
        val listed = official.mapTo(HashSet()) { it.id }
        val community = (tree[COMMUNITY] as? JsonArray).orEmpty().mapNotNull { element ->
            val pkg = communityPackage(element) ?: return@mapNotNull null
            pkg.takeIf { listed.add(it.id) }
        }
        return index.copy(packages = official + community, app = tree[APP]?.let(::appRelease))
    }

    /** [pkg] with its versions newest first and those that are not valid semver left out; null if none is left. */
    private fun sorted(pkg: CataloguePackage): CataloguePackage? {
        val versions = pkg.versions
            .mapNotNull { version -> SemVer.parseOrNull(version.version)?.let { it to version } }
            .sortedByDescending { it.first }
            .map { it.second }
        return if (versions.isEmpty()) null else pkg.copy(versions = versions)
    }

    /** The community package [element] describes, or null where it is one the app leaves out (see [parseIndex]). */
    private fun communityPackage(element: JsonElement): CataloguePackage? {
        val pkg = try {
            json.decodeFromJsonElement<CataloguePackage>(element)
        } catch (e: SerializationException) {
            return null
        } catch (e: IllegalArgumentException) {
            return null
        }
        if (!ManifestParser.isPackageId(pkg.id) || pkg.id.startsWith(RESERVED_PREFIX)) return null
        val signed = pkg.versions.filter { version ->
            version.signature?.let { Hex.decodeOrNull(it, Ed25519.SIGNATURE_BYTES) } != null
        }
        return sorted(pkg.copy(versions = signed, community = true))
    }

    /** The release `app` names, or null unless every part of it is one the app could act on. */
    private fun appRelease(element: JsonElement): AppRelease? {
        val release = try {
            json.decodeFromJsonElement<AppRelease>(element)
        } catch (e: SerializationException) {
            return null
        } catch (e: IllegalArgumentException) {
            return null
        }
        val usable = release.versionCode >= 1 &&
            release.versionName.isNotBlank() && release.versionName.length <= 64 &&
            release.url.startsWith("https://") &&
            release.size in 1..MAX_APP_BYTES &&
            Hex.isSha256(release.sha256) && Hex.isSha256(release.certSha256) &&
            release.minSdk >= 1
        return release.takeIf { usable }?.copy(notes = release.notes.map { it.trim() }.filter { it.isNotEmpty() })
    }

    private const val APP = "app"
    private const val COMMUNITY = "community"

    /** Ids that only the source's own packages may have: the agent lets only its built-in keys sign them. */
    const val RESERVED_PREFIX = "org.nbtapp."

    fun parseDetails(bytes: ByteArray): PackageDetails = decode(bytes, "package details")

    private inline fun <reified T> decode(bytes: ByteArray, what: String): T = try {
        json.decodeFromString<T>(bytes.decodeToString())
    } catch (e: SerializationException) {
        throw CatalogueFormatException("The $what is malformed", e)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.packages.ManifestParser
import com.nbtapps.store.model.AppRelease
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.ChangelogEntry
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageVersion
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
     *
     * A package's pre-release versions are published apart from its stable ones, under `channels.prerelease`,
     * since an app that knows no channels offers the highest version in `versions`. They go to
     * [CataloguePackage.prereleases] under the rules the stable ones are read by, signature and all, but apart from
     * the rest: a `channels` that is malformed, and a pre-release version that is, is left out, and the package is
     * kept with what can be read of it. Channels this app does not know are ignored, so that more can be published
     * later. A version in both lists is a stable one, and a package with no stable version is kept while it has a
     * pre-release.
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
        // The packages come out of the decoder in the order of the file's list, which is where their channels are.
        val published = (tree[PACKAGES] as? JsonArray).orEmpty()
        val official = index.packages.mapIndexedNotNull { i, pkg ->
            sorted(
                pkg.copy(
                    versions = pkg.versions.map { it.copy(signature = null) },
                    prereleases = prereleases(published.getOrNull(i)).map { it.copy(signature = null) },
                ),
            )
        }
        val listed = official.mapTo(HashSet()) { it.id }
        val community = (tree[COMMUNITY] as? JsonArray).orEmpty().mapNotNull { element ->
            val pkg = communityPackage(element) ?: return@mapNotNull null
            pkg.takeIf { listed.add(it.id) }
        }
        return index.copy(packages = official + community, app = tree[APP]?.let(::appRelease))
    }

    /**
     * [pkg] with its stable and its pre-release versions each newest first, those that are not valid semver left
     * out, and a pre-release that is also stable left out of the pre-releases; null if no version is left.
     */
    private fun sorted(pkg: CataloguePackage): CataloguePackage? {
        val versions = newestFirst(pkg.versions)
        val prereleases = newestFirst(pkg.prereleases).filter { (pre, _) -> versions.none { it.first.compareTo(pre) == 0 } }
        if (versions.isEmpty() && prereleases.isEmpty()) return null
        return pkg.copy(versions = versions.map { it.second }, prereleases = prereleases.map { it.second })
    }

    private fun newestFirst(versions: List<PackageVersion>): List<Pair<SemVer, PackageVersion>> = versions
        .mapNotNull { version -> SemVer.parseOrNull(version.version)?.let { it to version } }
        .sortedByDescending { it.first }

    /**
     * The versions the package [element] publishes under `channels.prerelease`, each read on its own: one that
     * cannot be read is left out, and so are all of them where `channels` or its `prerelease` is not as published.
     */
    private fun prereleases(element: JsonElement?): List<PackageVersion> {
        val channels = (element as? JsonObject)?.get(CHANNELS) as? JsonObject ?: return emptyList()
        val list = channels[PRERELEASE] as? JsonArray ?: return emptyList()
        return list.mapNotNull { decodeOrNull<PackageVersion>(it) }
    }

    /** [element] as a [T], or null where it is not one. */
    private inline fun <reified T> decodeOrNull(element: JsonElement): T? = try {
        json.decodeFromJsonElement<T>(element)
    } catch (e: SerializationException) {
        null
    } catch (e: IllegalArgumentException) {
        null
    }

    /** Those of [versions] with a usable signature of the source's: the only ones of a community package the app offers. */
    private fun signed(versions: List<PackageVersion>): List<PackageVersion> = versions.filter { version ->
        version.signature?.let { Hex.decodeOrNull(it, Ed25519.SIGNATURE_BYTES) } != null
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
        return sorted(pkg.copy(versions = signed(pkg.versions), prereleases = signed(prereleases(element)), community = true))
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
    private const val PACKAGES = "packages"
    private const val CHANNELS = "channels"
    private const val PRERELEASE = "prerelease"

    /** Ids that only the source's own packages may have: the agent lets only its built-in keys sign them. */
    const val RESERVED_PREFIX = "org.nbtapp."

    /**
     * Parses `details/<id>.json`. The notes of pre-release versions, under `channels.prerelease`, are read apart
     * from the rest, as the index's pre-release versions are: an entry that cannot be read is left out, and a
     * `channels` that is malformed leaves them all out without failing the file.
     */
    fun parseDetails(bytes: ByteArray): PackageDetails {
        val tree = try {
            json.parseToJsonElement(bytes.decodeToString())
        } catch (e: SerializationException) {
            throw CatalogueFormatException("The package details is malformed", e)
        }
        val details = try {
            json.decodeFromJsonElement<PackageDetails>(tree)
        } catch (e: SerializationException) {
            throw CatalogueFormatException("The package details is malformed", e)
        } catch (e: IllegalArgumentException) {
            throw CatalogueFormatException("The package details is malformed", e)
        }
        val channels = (tree as? JsonObject)?.get(CHANNELS) as? JsonObject
        val notes = (channels?.get(PRERELEASE) as? JsonArray).orEmpty().mapNotNull { decodeOrNull<ChangelogEntry>(it) }
        return details.copy(prereleaseChangelog = notes)
    }
}

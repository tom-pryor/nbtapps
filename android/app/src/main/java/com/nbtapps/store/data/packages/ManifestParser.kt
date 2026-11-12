// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.json.JsonProfileException
import com.nbtapps.store.data.json.StrictJson
import com.nbtapps.store.data.json.Utf8
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.ManifestFile
import com.nbtapps.store.model.ManifestHook
import com.nbtapps.store.model.ManifestLink
import com.nbtapps.store.model.ManifestSerialService
import com.nbtapps.store.model.PackageContent
import com.nbtapps.store.model.PackageDependency
import com.nbtapps.store.model.PackageManifest
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.SemVer
import com.nbtapps.store.model.VersionRange
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.longOrNull

/**
 * Reads `manifest.json` under the rules of `spec/manifest.schema.json` in the agent repo, plus the rules that
 * span fields and that a schema cannot express. It is the same walk as the agent's `agent/manifest.cpp`, and
 * it must give the same verdict: a manifest one of them takes and the other refuses is a bug in one of them.
 *
 * Unknown keys are refused inside `requires` and `install`, because an install key that is skipped could
 * place a file wrongly, and ignored everywhere else.
 */
object ManifestParser {
    const val SUPPORTED_MANIFEST_VERSION = 1
    const val MAX_MANIFEST_BYTES = 1024 * 1024
    const val MAX_PATH_BYTES = 255

    private val HOOK_NAMES = listOf("preInstall", "postInstall", "preRemove", "postRemove")
    private val REBOOT_VALUES = listOf("none", "hmi-restart", "required")

    /** The name the car has for its own serial service, which no package's service may take. */
    private const val CAR_SERIAL_SERVICE = "SPP"

    /** @throws PackageFormatException with the code `bad-manifest` or `unsupported-manifest`. */
    fun parse(bytes: ByteArray): PackageManifest {
        if (bytes.size > MAX_MANIFEST_BYTES) bad("is larger than 1 MiB")
        val root = try {
            StrictJson.parseObject(bytes)
        } catch (e: JsonProfileException) {
            bad("is not inside the JSON profile: ${e.message}")
        }
        val manifestVersion = root["manifestVersion"].integerOrNull() ?: bad("has no integer manifestVersion")
        if (manifestVersion != SUPPORTED_MANIFEST_VERSION.toLong()) {
            throw PackageFormatException(
                "Package format $manifestVersion isn't supported by this version of the app",
                PackageFormatException.UNSUPPORTED_MANIFEST,
            )
        }
        for (key in listOf("id", "name", "version", "contents")) {
            if (key !in root) bad("has no \"$key\"")
        }

        var requires = PackageRequirements()
        var depends = emptyList<PackageDependency>()
        var conflicts = emptyList<PackageDependency>()
        var contents = emptyList<PackageContent>()
        var install = Install()
        // Members are checked in the order of the text, as the agent does, so both name the same first fault.
        for ((key, value) in root) {
            when (key) {
                "id" -> text(value, "id", ::isPackageId)
                "name" -> text(value, "name", ::isName)
                "version" -> text(value, "version") { SemVer.parseOrNull(it) != null }
                "requires" -> requires = requires(value)
                "depends" -> depends = dependencies(value, "depends")
                "conflicts" -> conflicts = dependencies(value, "conflicts")
                "contents" -> contents = contents(value)
                "install" -> install = install(value)
            }
        }
        val manifest = PackageManifest(
            manifestVersion = manifestVersion.toInt(),
            id = root.getValue("id").string(),
            name = root.getValue("name").string(),
            version = root.getValue("version").string(),
            requires = requires,
            depends = depends,
            conflicts = conflicts,
            contents = contents,
            files = install.files,
            links = install.links,
            hooks = install.hooks,
            reboot = install.reboot,
            serialServices = install.serialServices,
        )
        crossFields(manifest)
        return manifest
    }

    private class Install(
        val files: List<ManifestFile> = emptyList(),
        val links: List<ManifestLink> = emptyList(),
        val hooks: Map<String, ManifestHook> = emptyMap(),
        val reboot: String = "none",
        val serialServices: List<ManifestSerialService> = emptyList(),
    )

    // ---- the schema, walked in the order of the text ----

    private fun requires(value: JsonElement): PackageRequirements {
        val obj = value as? JsonObject ?: bad("requires must be an object")
        var agent: String? = null
        for ((key, member) in obj) {
            if (key != "agent") unknown("requires", key)
            agent = text(member, "requires.agent", ::isRange)
        }
        return PackageRequirements(agent)
    }

    private fun dependencies(value: JsonElement, where: String): List<PackageDependency> {
        val array = value as? JsonArray ?: bad("$where must be an array")
        return array.mapIndexed { i, item ->
            val at = "$where[$i]"
            val obj = item as? JsonObject ?: bad("$at must be an object")
            if ("id" !in obj) bad("$at has no \"id\"")
            var id = ""
            var range: String? = null
            for ((key, member) in obj) {
                when (key) {
                    "id" -> id = text(member, "$at.id", ::isPackageId)
                    "version" -> range = text(member, "$at.version", ::isRange)
                }
            }
            PackageDependency(id, range)
        }
    }

    private fun contents(value: JsonElement): List<PackageContent> {
        val array = value as? JsonArray ?: bad("contents must be an array")
        return array.mapIndexed { i, item ->
            val at = "contents[$i]"
            val obj = item as? JsonObject ?: bad("$at must be an object")
            for (key in listOf("path", "size", "sha256")) {
                if (key !in obj) bad("$at has no \"$key\"")
            }
            var path = ""
            var size = 0L
            var sha256 = ""
            for ((key, member) in obj) {
                when (key) {
                    "path" -> path = text(member, "$at.path", ::isPath)
                    "size" -> size = integer(member, "$at.size", 0, 4294967294L)
                    "sha256" -> sha256 = text(member, "$at.sha256", Hex::isSha256)
                }
            }
            PackageContent(path, size, sha256)
        }
    }

    private fun install(value: JsonElement): Install {
        val obj = value as? JsonObject ?: bad("install must be an object")
        var files = emptyList<ManifestFile>()
        var links = emptyList<ManifestLink>()
        var hooks = emptyMap<String, ManifestHook>()
        var reboot = "none"
        var serialServices = emptyList<ManifestSerialService>()
        for ((key, member) in obj) {
            when (key) {
                "files" -> files = files(member)
                "links" -> links = links(member)
                "hooks" -> hooks = hooks(member)
                "reboot" -> {
                    reboot = (member as? JsonPrimitive)?.takeIf { it.isString }?.content ?: ""
                    if (reboot !in REBOOT_VALUES) bad("install.reboot must be one of: none, hmi-restart, required")
                }
                "serialServices" -> serialServices = serialServices(member)
                else -> unknown("install", key)
            }
        }
        return Install(files, links, hooks, reboot, serialServices)
    }

    private fun serialServices(value: JsonElement): List<ManifestSerialService> {
        val array = value as? JsonArray ?: bad("install.serialServices must be an array")
        return array.mapIndexed { i, item ->
            val at = "install.serialServices[$i]"
            val obj = item as? JsonObject ?: bad("$at must be an object")
            for (key in listOf("name", "program")) {
                if (key !in obj) bad("$at has no \"$key\"")
            }
            var name = ""
            var program = ""
            for ((key, member) in obj) {
                when (key) {
                    "name" -> name = text(member, "$at.name", ::isSerialServiceName)
                    "program" -> program = text(member, "$at.program", ::isProgramName)
                    else -> unknown(at, key)
                }
            }
            ManifestSerialService(name, program)
        }
    }

    private fun files(value: JsonElement): List<ManifestFile> {
        val array = value as? JsonArray ?: bad("install.files must be an array")
        return array.mapIndexed { i, item ->
            val at = "install.files[$i]"
            val obj = item as? JsonObject ?: bad("$at must be an object")
            for (key in listOf("src", "dest", "mode")) {
                if (key !in obj) bad("$at has no \"$key\"")
            }
            var src = ""
            var dest = ""
            var mode = ""
            var config = false
            var hasConfig = false
            var replaces: List<String>? = null
            for ((key, member) in obj) {
                when (key) {
                    "src" -> src = text(member, "$at.src", ::isSrc)
                    "dest" -> dest = text(member, "$at.dest", ::isDest)
                    "mode" -> mode = text(member, "$at.mode", ::isMode)
                    "config" -> {
                        config = (member as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull
                            ?: bad("$at.config must be true or false")
                        hasConfig = true
                    }
                    "replaces" -> replaces = replaces(member, "$at.replaces")
                    else -> unknown(at, key)
                }
            }
            if (src.endsWith("/") && (hasConfig || replaces != null)) bad("sets config or replaces on the folder $src")
            ManifestFile(src, dest, mode, config, replaces.orEmpty())
        }
    }

    private fun replaces(value: JsonElement, where: String): List<String> {
        val obj = value as? JsonObject ?: bad("$where must be an object")
        if ("sha256" !in obj) bad("$where has no \"sha256\"")
        var hashes = emptyList<String>()
        for ((key, member) in obj) {
            if (key != "sha256") unknown(where, key)
            val list = member as? JsonArray ?: bad("$where.sha256 must be an array")
            if (list.isEmpty()) bad("$where.sha256 needs at least one hash")
            hashes = list.mapIndexed { i, hash -> text(hash, "$where.sha256[$i]", Hex::isSha256) }
        }
        return hashes
    }

    private fun links(value: JsonElement): List<ManifestLink> {
        val array = value as? JsonArray ?: bad("install.links must be an array")
        return array.mapIndexed { i, item ->
            val at = "install.links[$i]"
            val obj = item as? JsonObject ?: bad("$at must be an object")
            for (key in listOf("path", "target")) {
                if (key !in obj) bad("$at has no \"$key\"")
            }
            var path = ""
            var target = ""
            for ((key, member) in obj) {
                when (key) {
                    "path" -> path = text(member, "$at.path", ::isLinkPath)
                    "target" -> target = text(member, "$at.target", ::isLinkTarget)
                    else -> unknown(at, key)
                }
            }
            ManifestLink(path, target)
        }
    }

    private fun hooks(value: JsonElement): Map<String, ManifestHook> {
        val obj = value as? JsonObject ?: bad("install.hooks must be an object")
        val out = LinkedHashMap<String, ManifestHook>()
        for ((name, member) in obj) {
            if (name !in HOOK_NAMES) unknown("install.hooks", name)
            val at = "install.hooks.$name"
            val hook = member as? JsonObject ?: bad("$at must be an object")
            for (key in listOf("script", "timeout")) {
                if (key !in hook) bad("$at has no \"$key\"")
            }
            var script = ""
            var timeout = 0L
            for ((key, field) in hook) {
                when (key) {
                    "script" -> script = text(field, "$at.script", ::isPath)
                    "timeout" -> timeout = integer(field, "$at.timeout", 1, 600)
                    else -> unknown(at, key)
                }
            }
            out[name] = ManifestHook(script, timeout.toInt())
        }
        return out
    }

    // ---- the rules that span fields ----

    private fun crossFields(manifest: PackageManifest) {
        val paths = HashMap<String, PackageContent>()
        for (content in manifest.contents) {
            if (content.path == PackageArchive.MANIFEST_NAME || content.path == PackageArchive.SIGNATURE_NAME) {
                bad("lists ${content.path} in contents")
            }
            if (content.path.encodeToByteArray().size > MAX_PATH_BYTES) bad("lists a path longer than 255 bytes")
            if (paths.put(content.path, content) != null) bad("lists ${content.path} twice")
        }

        val placed = HashSet<String>()
        fun place(dest: String) {
            if (dest.encodeToByteArray().size > MAX_PATH_BYTES) bad("places a path longer than 255 bytes: $dest")
            if (!placed.add(dest)) bad("places $dest twice")
        }
        for (file in manifest.files) {
            if (file.src.endsWith("/") != file.dest.endsWith("/")) {
                bad("maps ${file.src} to ${file.dest}: a folder goes to a folder and a file to a file")
            }
            if (file.src.endsWith("/")) {
                val inside = paths.keys.filter { it.startsWith(file.src) }
                if (inside.isEmpty()) bad("installs the folder ${file.src}, which holds nothing listed in contents")
                inside.forEach { place(file.dest + it.removePrefix(file.src)) }
            } else {
                if (file.src !in paths) bad("installs ${file.src}, which is not listed in contents")
                place(file.dest)
            }
        }
        manifest.links.forEach { place(it.path) }
        for (hook in manifest.hooks.values) {
            if (hook.script !in paths) bad("names ${hook.script} as a hook, which is not listed in contents")
        }
        val services = HashSet<String>()
        for (service in manifest.serialServices) {
            if (!services.add(service.name)) bad("declares the serial service ${service.name} twice")
        }
    }

    // ---- the string forms of the schema ----

    private fun noControls(text: String) = text.none { it < ' ' || it == '\u007F' }

    /** Segments separated by single slashes: none empty, `.` or `..`, none with a backslash or a control character. */
    private fun segments(text: String, trailingSlash: Boolean): Boolean {
        val path = if (trailingSlash && text.endsWith("/")) text.dropLast(1) else text
        if (path.isEmpty()) return false
        return path.split('/').none { it.isEmpty() || it == "." || it == ".." || it.any { c -> c == '\\' || c < ' ' } }
    }

    /** Whether [text] may name an entry of the archive: a relative path, with a slash at the end for a folder. */
    internal fun isRelativePath(text: String, folder: Boolean): Boolean = segments(text, folder)

    /** Two or more labels joined by dots; a label is runs of `[a-z0-9]` joined by single hyphens. */
    internal fun isPackageId(text: String): Boolean {
        if (text.length > 128) return false
        val labels = text.split('.')
        return labels.size >= 2 && labels.all { label ->
            label.isNotEmpty() && label.split('-').all { run -> run.isNotEmpty() && run.all { it in 'a'..'z' || it in '0'..'9' } }
        }
    }

    internal fun isName(text: String): Boolean = Utf8.length(text) in 1..64 && noControls(text)

    private fun isRange(text: String): Boolean = VersionRange.parseOrNull(text) != null

    /** A relative path inside the archive. */
    internal fun isPath(text: String): Boolean = Utf8.length(text) <= 255 && segments(text, trailingSlash = false)

    private fun isSrc(text: String): Boolean = Utf8.length(text) <= 255 && segments(text, trailingSlash = true)

    private fun isDest(text: String): Boolean {
        if (Utf8.length(text) > 255) return false
        for (prefix in listOf("\${app}/", "\${data}/", "\${plugins}/")) {
            if (text.startsWith(prefix)) return text.length == prefix.length || segments(text.substring(prefix.length), true)
        }
        return text.startsWith("/") && segments(text.substring(1), true)
    }

    private fun isLinkPath(text: String): Boolean {
        if (Utf8.length(text) > 255) return false
        val rest = listOf("\${app}", "\${data}", "\${plugins}").firstOrNull { text.startsWith(it) }?.let { text.substring(it.length) } ?: text
        return rest.startsWith("/") && segments(rest.substring(1), false)
    }

    private fun isLinkTarget(text: String): Boolean = Utf8.length(text) in 1..255 && noControls(text)

    /** 1 to 32 of `[A-Za-z0-9_-]`, and not a name that is taken: the agent's own service or the car's. */
    internal fun isSerialServiceName(text: String): Boolean =
        text.length in 1..32 && text.all { it in 'A'..'Z' || it in 'a'..'z' || it in '0'..'9' || it == '_' || it == '-' } &&
            text != ProtocolConstants.SERIAL_SERVICE_NAME && text != CAR_SERIAL_SERVICE

    /** A file name of 1 to 64 of `[A-Za-z0-9._+-]`, and not `.` or `..`. */
    internal fun isProgramName(text: String): Boolean =
        text.length in 1..64 && text != "." && text != ".." &&
            text.all { it in 'A'..'Z' || it in 'a'..'z' || it in '0'..'9' || it == '.' || it == '_' || it == '+' || it == '-' }

    /** `0` and three octal digits. */
    private fun isMode(text: String): Boolean = text.length == 4 && text[0] == '0' && text.drop(1).all { it in '0'..'7' }

    // ---- values ----

    private fun JsonElement?.integerOrNull(): Long? = (this as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull

    private fun JsonElement.string(): String = (this as JsonPrimitive).content

    private fun text(value: JsonElement, where: String, form: (String) -> Boolean): String {
        val text = (value as? JsonPrimitive)?.takeIf { it.isString }?.content ?: bad("$where must be a string")
        if (!form(text)) bad("$where has a form or a length that is not allowed")
        return text
    }

    private fun integer(value: JsonElement, where: String, low: Long, high: Long): Long {
        val number = value.integerOrNull() ?: bad("$where must be an integer")
        if (number < low || number > high) bad("$where is out of range")
        return number
    }

    private fun bad(what: String): Nothing =
        throw PackageFormatException("The package's manifest isn't valid: it $what", PackageFormatException.BAD_MANIFEST)

    private fun unknown(where: String, key: String): Nothing = throw PackageFormatException(
        "The package's manifest uses \"$key\" in $where, which this version of the app doesn't know",
        PackageFormatException.UNSUPPORTED_MANIFEST,
    )
}

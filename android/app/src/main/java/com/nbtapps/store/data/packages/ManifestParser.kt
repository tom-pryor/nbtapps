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
import com.nbtapps.store.model.PackageSettings
import com.nbtapps.store.model.SettingApplies
import com.nbtapps.store.model.SettingChoice
import com.nbtapps.store.model.SettingDefault
import com.nbtapps.store.model.SettingOption
import com.nbtapps.store.model.SettingType
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
 * place a file wrongly, and ignored everywhere else, a `logs` entry, `settings` and its options included. An option
 * of a type this app does not know is skipped, with only its `key` and `label` checked.
 */
object ManifestParser {
    const val SUPPORTED_MANIFEST_VERSION = 1
    const val MAX_MANIFEST_BYTES = 1024 * 1024
    const val MAX_PATH_BYTES = 255

    private val HOOK_NAMES = listOf("preInstall", "postInstall", "preRemove", "postRemove")
    private val REBOOT_VALUES = listOf("none", "hmi-restart", "required")

    /** The name the car has for its own serial service, which no package's service may take. */
    private const val CAR_SERIAL_SERVICE = "SPP"

    /** Entries in `logs`. */
    const val MAX_LOGS = 16

    /** Options in `settings`, those of types this app does not know included. */
    const val MAX_SETTINGS = 64

    /** Values of a `select` option. */
    const val MAX_SETTING_VALUES = 16

    /** UTF-8 in one option's strings, so that one option fits a 4,096-byte serial frame when the agent sends it. */
    const val MAX_SETTING_BYTES = 1024

    /** The limits of an `int` option's numbers. */
    private const val INT_MIN = -2147483648L
    private const val INT_MAX = 2147483647L

    /** The agent's own folders, which no log path may name: its state, with the pairing keys, and its RAM folder. */
    private val AGENT_FOLDERS = listOf("/tmp/nbtapp", "/var/opt/nbtapp")

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
        var logs = emptyList<String>()
        var settings: SettingsWalk? = null
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
                "logs" -> logs = logs(value)
                "settings" -> settings = settings(value)
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
            logs = logs,
            settings = settings?.let(::settingsRules),
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

    /** `logs`: at most 16 entries, each open, with a `path`. */
    private fun logs(value: JsonElement): List<String> {
        val array = value as? JsonArray ?: bad("logs must be an array")
        if (array.size > MAX_LOGS) bad("logs has more than $MAX_LOGS entries")
        return array.mapIndexed { i, item ->
            val at = "logs[$i]"
            val obj = item as? JsonObject ?: bad("$at must be an object")
            val path = obj["path"] ?: bad("$at has no \"path\"")
            text(path, "$at.path", ::isLogPath)
        }
    }

    /** `settings` as the walk finds it: the file, and per option what every option has and the object for the rest. */
    private class SettingsWalk(val file: String, val options: List<PendingSetting>)

    private class PendingSetting(val at: String, val key: String, val label: String, val type: String, val obj: JsonObject)

    /**
     * `settings`: open, with `file` and 1 to 64 options. Here only what every option has, whatever its type; the
     * rest waits for [settingsRules], after the walk, as the agent and `nbtpkg.py` do it, so that a key refused in
     * `install` further down the text still decides the code.
     */
    private fun settings(value: JsonElement): SettingsWalk {
        val obj = value as? JsonObject ?: bad("settings must be an object")
        if ("options" !in obj) bad("settings has no \"options\"")
        var file = PackageSettings.DEFAULT_FILE
        var options = emptyList<PendingSetting>()
        for ((key, member) in obj) {
            when (key) {
                "file" -> file = text(member, "settings.file", ::isSettingsFile)
                "options" -> {
                    val array = member as? JsonArray ?: bad("settings.options must be an array")
                    if (array.isEmpty()) bad("settings.options needs at least one option")
                    if (array.size > MAX_SETTINGS) bad("settings.options has more than $MAX_SETTINGS options")
                    options = array.mapIndexed { i, item ->
                        val at = "settings.options[$i]"
                        val option = item as? JsonObject ?: bad("$at must be an object")
                        for (required in listOf("key", "label", "type")) {
                            if (required !in option) bad("$at has no \"$required\"")
                        }
                        var settingKey = ""
                        var label = ""
                        var type = ""
                        for ((name, field) in option) {
                            when (name) {
                                "key" -> settingKey = text(field, "$at.key", ::isSettingKey)
                                "label" -> label = text(field, "$at.label", ::isName)
                                "type" -> type = text(field, "$at.type") { true }
                            }
                        }
                        PendingSetting(at, settingKey, label, type, option)
                    }
                }
            }
        }
        return SettingsWalk(file, options)
    }

    /**
     * Each option of a known type held to its type's definition, and the rules that span fields: no key twice, those
     * of options of unknown types included, and at most [MAX_SETTING_BYTES] of UTF-8 in an option's strings.
     */
    private fun settingsRules(walk: SettingsWalk): PackageSettings {
        val keys = HashSet<String>()
        val options = ArrayList<SettingOption>()
        for (pending in walk.options) {
            if (!keys.add(pending.key)) bad("names the setting ${pending.key} twice")
            val type = SettingType.of(pending.type)
            val option = type?.let { setting(pending, it) }
            var bytes = utf8Bytes(pending.key) + utf8Bytes(pending.label)
            if (option != null) {
                bytes += listOfNotNull(option.description, option.unset, option.unit).sumOf(::utf8Bytes)
                bytes += ((option.default as? SettingDefault.Text)?.value?.let(::utf8Bytes) ?: 0)
                bytes += option.values.sumOf { utf8Bytes(it.value) + utf8Bytes(it.label) }
            }
            if (bytes > MAX_SETTING_BYTES) bad("gives the setting ${pending.key} more than $MAX_SETTING_BYTES bytes of text")
            if (option != null) options.add(option)
        }
        return PackageSettings(walk.file, options)
    }

    /** One option of a known type: its type's members in the order of the text, then the rules across them. */
    private fun setting(pending: PendingSetting, type: SettingType): SettingOption {
        val at = pending.at
        val obj = pending.obj
        val required = when (type) {
            SettingType.BOOL -> emptyList()
            SettingType.INT -> listOf("min", "max")
            SettingType.TEXT -> listOf("maxLength")
            SettingType.SELECT -> listOf("values")
        }
        for (key in required) {
            if (key !in obj) bad("$at has no \"$key\"")
        }
        var description: String? = null
        var unset: String? = null
        var applies = SettingApplies.NOW
        var advanced = false
        var default: SettingDefault? = null
        var min = 0L
        var max = 0L
        var unit: String? = null
        var maxLength = 0
        var anyCharacters = false
        var values = emptyList<SettingChoice>()
        for ((key, member) in obj) {
            val where = "$at.$key"
            when {
                key == "description" -> description = text(member, where, ::isSettingDescription)
                key == "unset" -> unset = text(member, where, ::isName)
                key == "applies" -> applies = (member as? JsonPrimitive)?.takeIf { it.isString }?.let { SettingApplies.of(it.content) }
                    ?: bad("$where must be one of: now, app-start, hmi-restart, reboot")
                key == "advanced" -> advanced = boolean(member, where)
                key == "default" -> default = when (type) {
                    SettingType.BOOL -> SettingDefault.Bool(boolean(member, where))
                    SettingType.INT -> SettingDefault.Number(integer(member, where, INT_MIN, INT_MAX))
                    SettingType.TEXT, SettingType.SELECT -> SettingDefault.Text(text(member, where) { true })
                }
                type == SettingType.INT && key == "min" -> min = integer(member, where, INT_MIN, INT_MAX)
                type == SettingType.INT && key == "max" -> max = integer(member, where, INT_MIN, INT_MAX)
                type == SettingType.INT && key == "unit" -> unit = text(member, where, ::isSettingUnit)
                type == SettingType.TEXT && key == "maxLength" ->
                    maxLength = integer(member, where, 1, ProtocolConstants.SETTINGS_MAX_VALUE_BYTES.toLong()).toInt()
                type == SettingType.TEXT && key == "characters" -> anyCharacters =
                    when ((member as? JsonPrimitive)?.takeIf { it.isString }?.content) {
                        "safe" -> false
                        "any" -> true
                        else -> bad("$where must be one of: safe, any")
                    }
                type == SettingType.SELECT && key == "values" -> values = choices(member, where)
            }
        }

        val key = pending.key
        if (default != null && unset != null) bad("gives the setting $key both a default and an unset text")
        when (type) {
            SettingType.INT -> {
                if (min > max) bad("gives the setting $key a min above its max")
                val number = (default as? SettingDefault.Number)?.value
                if (number != null && number !in min..max) bad("gives the setting $key a default outside its limits")
            }
            SettingType.TEXT -> {
                val text = (default as? SettingDefault.Text)?.value
                if (text != null && !isSettingText(text, maxLength, anyCharacters)) bad("gives the setting $key a default it does not take")
            }
            SettingType.SELECT -> {
                if (values.map { it.value }.toSet().size != values.size) bad("gives the setting $key a value twice")
                val text = (default as? SettingDefault.Text)?.value
                if (text != null && values.none { it.value == text }) {
                    bad("gives the setting $key a default that is not one of its values")
                }
            }
            SettingType.BOOL -> Unit
        }
        return SettingOption(
            key = key,
            label = pending.label,
            type = type,
            description = description,
            min = min,
            max = max,
            unit = unit,
            maxLength = maxLength,
            anyCharacters = anyCharacters,
            values = values,
            default = default,
            unset = unset,
            applies = applies,
            advanced = advanced,
        )
    }

    /** A `select` option's `values`: 2 to 16, each open, with a `value` and a `label`. */
    private fun choices(value: JsonElement, where: String): List<SettingChoice> {
        val array = value as? JsonArray ?: bad("$where must be an array")
        if (array.size < 2) bad("$where needs at least two values")
        if (array.size > MAX_SETTING_VALUES) bad("$where has more than $MAX_SETTING_VALUES values")
        return array.mapIndexed { i, item ->
            val at = "$where[$i]"
            val obj = item as? JsonObject ?: bad("$at must be an object")
            for (key in listOf("value", "label")) {
                if (key !in obj) bad("$at has no \"$key\"")
            }
            var choice = ""
            var label = ""
            for ((key, member) in obj) {
                when (key) {
                    "value" -> choice = text(member, "$at.value", ::isChoiceValue)
                    "label" -> label = text(member, "$at.label", ::isName)
                }
            }
            SettingChoice(choice, label)
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
        val logs = HashSet<String>()
        for (log in manifest.logs) {
            if (!logs.add(log)) bad("names the log $log twice")
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

    /** A variable a `logs` entry may start with: `${`, a name of `[a-z0-9_]`, `}`. */
    private val LOG_VARIABLE = Regex("""\$\{[a-z0-9_]+}""")

    /**
     * A `logs` entry's path of the right shape (`$defs/logShape`): a variable `${name}` or nothing, then segments that
     * each start with a slash, the wildcards `*` and `?` only in the last, and an absolute path outside the agent's
     * own folders. Which folders exist is the agent's to know: one it does not know is skipped there, so the app takes
     * every folder and a new one needs no change here (`spec/log-paths.vectors` in the agent repo has the cases).
     */
    internal fun isLogPath(text: String): Boolean {
        if (Utf8.length(text) > 255) return false
        val rest = when {
            text.startsWith("/") -> {
                if (AGENT_FOLDERS.any { text == it || text.startsWith("$it/") }) return false
                text
            }
            else -> {
                val variable = LOG_VARIABLE.matchAt(text, 0) ?: return false
                text.substring(variable.range.last + 1)
            }
        }
        if (!rest.startsWith("/") || !segments(rest.substring(1), trailingSlash = false)) return false
        return rest.substringBeforeLast('/').none { it == '*' || it == '?' }
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

    /** `settings.file`: `${data}/` and a relative path, no slash at the end, at most 255 characters. */
    internal fun isSettingsFile(text: String): Boolean =
        Utf8.length(text) <= 255 && text.startsWith("\${data}/") && segments(text.substring("\${data}/".length), trailingSlash = false)

    /** An option's key: `[A-Z][A-Z0-9_]{0,63}`. */
    internal fun isSettingKey(text: String): Boolean =
        text.length in 1..64 && text[0] in 'A'..'Z' && text.all { it in 'A'..'Z' || it in '0'..'9' || it == '_' }

    private fun isSettingDescription(text: String): Boolean = Utf8.length(text) <= 500 && noControls(text)

    private fun isSettingUnit(text: String): Boolean = Utf8.length(text) in 1..16 && noControls(text)

    /** A `select` option's value: 1 to 64 of `[A-Za-z0-9._-]`. */
    private fun isChoiceValue(text: String): Boolean =
        text.length in 1..64 && text.all { it in 'A'..'Z' || it in 'a'..'z' || it in '0'..'9' || it == '.' || it == '_' || it == '-' }

    /**
     * Whether a `text` option takes [value]: 1 to [maxLength] bytes of UTF-8, of the safe set `A-Z a-z 0-9 . _ : / @ + , -`
     * or, with [anyCharacters], with no control character and no space at its start or end.
     */
    fun isSettingText(value: String, maxLength: Int, anyCharacters: Boolean): Boolean {
        if (value.isEmpty() || utf8Bytes(value) > maxLength) return false
        if (anyCharacters) return noControls(value) && !value.startsWith(' ') && !value.endsWith(' ')
        return value.all { it in 'A'..'Z' || it in 'a'..'z' || it in '0'..'9' || it in "._:/@+,-" }
    }

    private fun utf8Bytes(text: String): Int = text.encodeToByteArray().size

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

    private fun boolean(value: JsonElement, where: String): Boolean =
        (value as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull ?: bad("$where must be true or false")

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

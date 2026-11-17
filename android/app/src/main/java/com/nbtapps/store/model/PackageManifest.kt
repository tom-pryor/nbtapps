// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

/**
 * The `manifest.json` at the start of a `.nbtpkg`, as `ManifestParser` reads it. The phone acts on the
 * identity and the dependencies; the install plan is the agent's to carry out, and is read here only so the
 * app can say what a package will do before the user installs it.
 */
data class PackageManifest(
    val manifestVersion: Int,
    val id: String,
    val name: String,
    val version: String,
    val requires: PackageRequirements = PackageRequirements(),
    val depends: List<PackageDependency> = emptyList(),
    val conflicts: List<PackageDependency> = emptyList(),
    /** Every file in the archive except the manifest and its signature. */
    val contents: List<PackageContent>,
    val files: List<ManifestFile> = emptyList(),
    val links: List<ManifestLink> = emptyList(),
    /** Keyed by `preInstall`, `postInstall`, `preRemove` and `postRemove`. */
    val hooks: Map<String, ManifestHook> = emptyMap(),
    /** `none`, `hmi-restart` or `required`. */
    val reboot: String = "none",
    /** The Bluetooth serial services the package's programs register on the car, for the agent to clear up after them. */
    val serialServices: List<ManifestSerialService> = emptyList(),
    /** `logs[].path`: the package's log files, which the agent sends a controller that asks with `files.fetch`. */
    val logs: List<String> = emptyList(),
    /** `settings`: what a phone shows and changes for the package; null when the manifest names none. */
    val settings: PackageSettings? = null,
) {
    /**
     * The risk flags, worked out from the install plan the way the agent does it. The app shows these, not
     * the ones a catalogue publishes, once it holds the package: a source cannot understate them.
     */
    val flags: List<String>
        get() = buildList {
            if (hooks.isNotEmpty()) add(RiskFlag.RUNS_SCRIPTS.id)
            val system = files.any { !inOwnDirectories(it.dest) || it.replaces.isNotEmpty() } ||
                links.any { !inOwnDirectories(it.path) }
            if (system) add(RiskFlag.MODIFIES_SYSTEM.id)
            if (reboot != "none") add(RiskFlag.NEEDS_REBOOT.id)
        }

    private fun inOwnDirectories(dest: String): Boolean = OWN_DIRECTORIES.any { dest.startsWith(it) }

    private companion object {
        val OWN_DIRECTORIES = listOf("\${app}/", "\${data}/", "\${plugins}/")
    }
}

/** One file of a package, as its manifest pins it. */
data class PackageContent(
    /** Path inside the archive, such as `payload/bin/hello`. */
    val path: String,
    /** Size of the uncompressed file in bytes. */
    val size: Long,
    /** SHA-256 of the uncompressed file, in hex. */
    val sha256: String,
)

/** One entry of `install.files`: a file or a folder of the archive and where it goes on the head unit. */
data class ManifestFile(
    val src: String,
    val dest: String,
    val mode: String,
    val config: Boolean = false,
    /** The acceptable hashes of the stock file this entry replaces; empty when it replaces none. */
    val replaces: List<String> = emptyList(),
)

data class ManifestLink(val path: String, val target: String)

data class ManifestHook(val script: String, val timeoutSeconds: Int)

/**
 * One entry of `install.serialServices`: a Bluetooth serial service that a program of the package registers with
 * the car while it runs. The car keeps a registration after its program has gone, and the agent removes it then.
 */
data class ManifestSerialService(
    /** The name the service is registered under. */
    val name: String,
    /** The file name of the program that registers it. */
    val program: String,
)

/**
 * A manifest's `settings`: the file the agent keeps them in, one `KEY=value` a line, and the options a phone shows.
 * Options of a type this app does not know are left out, so [options] may be empty.
 */
data class PackageSettings(
    /** As the manifest writes it, `${data}/` and a relative path; [DEFAULT_FILE] when it names none. */
    val file: String = DEFAULT_FILE,
    /** In the order of the manifest. */
    val options: List<SettingOption>,
) {
    companion object {
        const val DEFAULT_FILE = "\${data}/settings"
    }
}

/** The types of option this app knows, by the name the manifest gives them. */
enum class SettingType(val id: String) {
    BOOL("bool"),
    INT("int"),
    TEXT("text"),
    SELECT("select"),
    ;

    companion object {
        fun of(id: String): SettingType? = entries.firstOrNull { it.id == id }
    }
}

/** When a change to an option takes effect. */
enum class SettingApplies(val id: String) {
    NOW("now"),
    APP_START("app-start"),
    HMI_RESTART("hmi-restart"),
    REBOOT("reboot"),
    ;

    companion object {
        fun of(id: String): SettingApplies? = entries.firstOrNull { it.id == id }
    }
}

/** A default, of the kind its option's type has. */
sealed interface SettingDefault {
    data class Bool(val value: Boolean) : SettingDefault
    data class Number(val value: Long) : SettingDefault
    /** A `text` option's default, or a `select` option's, which is one of its values. */
    data class Text(val value: String) : SettingDefault
}

/** One value a `select` option may have: as it stands in the settings file, and what the phone shows for it. */
data class SettingChoice(val value: String, val label: String)

/**
 * One option of `settings.options`, of a known type. Members another type has keep the values they are made with.
 */
data class SettingOption(
    /** The name in the settings file: `[A-Z][A-Z0-9_]{0,63}`. */
    val key: String,
    val label: String,
    val type: SettingType,
    val description: String? = null,
    /** `int`: the limits, both included, and what is shown beside the number. */
    val min: Long = 0,
    val max: Long = 0,
    val unit: String? = null,
    /** `text`: the most bytes of UTF-8 a value has, and whether it may hold any characters or the safe set only. */
    val maxLength: Int = 0,
    val anyCharacters: Boolean = false,
    /** `select`: in the order of the manifest. */
    val values: List<SettingChoice> = emptyList(),
    val default: SettingDefault? = null,
    /** What the phone shows when the option is not set. Never given together with [default]. */
    val unset: String? = null,
    val applies: SettingApplies = SettingApplies.NOW,
    val advanced: Boolean = false,
)

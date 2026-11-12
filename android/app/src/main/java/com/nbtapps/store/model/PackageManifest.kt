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

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

/**
 * The app's own words for the protocol's codes. The agent sends an English message with every error, but that
 * one is for logs: what the user reads comes from here, so it can say what to do next.
 */
object AgentText {

    /** For the `error` of a response. */
    fun request(code: String): String = when (code) {
        "busy" -> "The head unit is busy with another job"
        "not-found" -> "The head unit no longer has that plan. Plan it again."
        "plan-stale" -> "The head unit changed since the plan was made. Plan it again."
        "too-late" -> "The install has already begun, so it can't be stopped"
        "unsupported" -> "The head unit's agent can't do that yet"
        "unsupported-version" -> "The head unit's agent speaks a version of the protocol this app doesn't"
        "forbidden" -> "The head unit doesn't allow that"
        "bad-request", "bad-state", "out-of-range" -> "The head unit didn't accept the request"
        "unauthenticated", "auth-failed" -> "The head unit doesn't know this phone"
        "rejected" -> "It was declined on the car's screen"
        "try-later" -> "Try again in a moment"
        else -> "The head unit's agent ran into a fault"
    }

    /** Why the agent cannot be started with the car, for the `reason` of [Autostart]: one of [AutostartReason]'s. */
    fun autostartReason(reason: String?): String = when (reason) {
        AutostartReason.PLUGIN_MISSING -> "The iDrive plugin is what starts the agent with the car, and it isn't installed."
        AutostartReason.PLUGIN_TOO_OLD -> "The iDrive plugin on the head unit is too old to start the agent. Update it."
        AutostartReason.NO_LAUNCHER -> "This agent wasn't installed in a way that lets it be started with the car."
        else -> "The head unit has nothing that would start the agent with the car."
    }

    /** For the `error` of `autostart.set`. */
    fun autostart(code: String): String = when (code) {
        AutostartReason.PLUGIN_MISSING, AutostartReason.PLUGIN_TOO_OLD, AutostartReason.NO_LAUNCHER -> autostartReason(code)
        "busy" -> "The agent is finishing its own update. Try again in a moment."
        "io-error" -> "The head unit couldn't write a file"
        "unsupported" -> "The head unit's agent is too old to be switched from the phone"
        else -> request(code)
    }

    /** Where on the car's screen a phone is paired: what the agent repo's plugin calls its menu and the row in it. */
    const val PAIR_ON_CAR = "On the car's screen, open NBTApp (in the Media/Radio menu) and choose Pair a phone."

    /** For the `error` of `pairing.issue`, which carries the code from the car's screen. */
    fun pairing(code: String): String = when (code) {
        "forbidden" -> "The car isn't showing a code just now. $PAIR_ON_CAR"
        "rejected" -> "That isn't the code on the car's screen"
        "bad-request" -> "A code is six digits"
        "unsupported" -> "The head unit's agent is too old to pair a phone"
        else -> request(code)
    }

    /** For a `problems` entry of a plan and for the `error` of a failed step. [id] is the package it is about. */
    fun problem(code: String, id: String): String {
        val pkg = id.ifEmpty { "A package" }
        return when (code) {
            "no-offer" -> "$pkg isn't on the phone yet. Download it first."
            "fetch-failed" -> "$pkg couldn't be transferred from the phone"
            "bad-manifest", "offer-mismatch", "bad-archive" -> "$pkg's archive is damaged or isn't what it claims to be"
            "unsupported-manifest", "agent-too-old" -> "$pkg needs a newer agent than the head unit has"
            "bad-signature" -> "$pkg isn't signed by a source the head unit trusts"
            "reserved-id" -> "$pkg uses a name reserved for the official source"
            "key-mismatch" -> "$pkg is installed from another source, and only that source can update it"
            "downgrade" -> "A newer version of $pkg is already installed"
            "already-installed" -> "That version of $pkg is already installed"
            "not-installed" -> "$pkg isn't installed"
            "required-by" -> "$pkg is needed by another package"
            "conflict" -> "$pkg can't be installed alongside another package"
            "file-owned" -> "$pkg would overwrite a file that belongs to another package"
            "stock-mismatch" -> "$pkg doesn't fit this head unit's firmware: a file it replaces isn't the one it expects"
            "hmi-id-taken" -> "$pkg uses a menu id that is already taken on this head unit"
            "no-space" -> "There isn't enough free space on the head unit"
            "hook-failed" -> "A script of $pkg failed"
            "hook-timeout" -> "A script of $pkg took too long and was stopped"
            "io-error" -> "The head unit couldn't write a file"
            "interrupted" -> "The head unit lost power during the install and undid it"
            else -> "The head unit's agent ran into a fault with $pkg"
        }
    }

    fun phase(phase: String?): String = when (phase) {
        "fetch" -> "Transferring"
        "verify" -> "Checking"
        "pre-install" -> "Preparing"
        "backup" -> "Backing up"
        "files" -> "Copying files"
        "post-install" -> "Finishing"
        "record" -> "Recording"
        "pre-remove" -> "Preparing to remove"
        "remove" -> "Removing files"
        "post-remove" -> "Cleaning up"
        "rollback" -> "Undoing"
        else -> "Working"
    }

    fun state(state: String): String = when (state) {
        JobState.FETCHING -> "Transferring to the head unit"
        JobState.PAUSED -> "Paused: the link dropped"
        JobState.INSTALLING -> "Installing"
        JobState.RESTARTING -> "The agent is restarting"
        JobState.SUCCEEDED -> "Done"
        JobState.FAILED -> "Failed"
        JobState.CANCELLED -> "Stopped"
        else -> state
    }

    /** For the `reason` of `link.closing`: why the agent ended the link, and what there is to do about it. */
    fun closing(reason: String): String = when (reason) {
        LinkClosing.RESTARTING -> "The head unit's agent is restarting"
        LinkClosing.STOPPING -> "The agent on the head unit was stopped"
        LinkClosing.FORGOTTEN -> "The head unit no longer knows this phone. Pair it again."
        LinkClosing.REPLACED -> "The head unit took a newer link from this phone"
        LinkClosing.SILENT -> "The head unit stopped hearing this phone. Connect again."
        else -> "The head unit's agent closed the link"
    }

    /** What a step does, such as "Upgrade Hello NBT 1.1.0 to 1.2.0". [name] is the package's display name. */
    fun step(action: String, name: String, version: String, oldVersion: String?): String = when (action) {
        "remove" -> "Remove $name $version".trim()
        "upgrade" -> if (oldVersion != null) "Upgrade $name $oldVersion to $version" else "Upgrade $name to $version"
        else -> "Install $name $version"
    }

    fun stepResult(result: String): String = when (result) {
        "done" -> "Done"
        "failed" -> "Failed and undone"
        "broken" -> "Failed, and could not be fully undone"
        "skipped" -> "Not attempted"
        else -> result
    }

    fun restart(kind: String): String? = when (kind) {
        RestartKind.HMI -> "iDrive has to restart to finish"
        RestartKind.REBOOT -> "The head unit has to reboot to finish"
        else -> null
    }
}

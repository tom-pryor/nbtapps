// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import androidx.core.net.toUri

/**
 * The address of the install guide, whose steps 1 to 3 put the agent on the car from a computer. Every page that
 * points to the guide takes it from here.
 */
const val INSTALL_GUIDE_URL = "https://github.com/tom-pryor/nbtapps/blob/main/headunit/INSTALL.md"

/**
 * The address of the installer: a web page that puts the agent on the car by itself, over an ENET cable, from an
 * Android phone or a computer. It can also switch on the SSH access that the install guide starts from.
 */
const val AGENT_INSTALLER_URL = "https://ssh.bavarian-aa.uk/"

/** Opens the installer in the phone's browser. */
internal fun openAgentInstaller(context: Context) = start(context, Intent(Intent.ACTION_VIEW, AGENT_INSTALLER_URL.toUri()))

/**
 * Hands the guide's address to Android's share sheet, as plain text, so the user can send it on to the computer the
 * agent is installed from.
 */
internal fun sendInstallGuide(context: Context) {
    val send = Intent(Intent.ACTION_SEND)
        .setType("text/plain")
        .putExtra(Intent.EXTRA_TEXT, INSTALL_GUIDE_URL)
    start(context, Intent.createChooser(send, null))
}

/** Opens the guide in the phone's browser. */
internal fun openInstallGuide(context: Context) = start(context, Intent(Intent.ACTION_VIEW, INSTALL_GUIDE_URL.toUri()))

/**
 * Opens [address] in the phone's browser, as the guide is opened: a package's website, which its page has checked
 * is an https one.
 */
internal fun openWebPage(context: Context, address: String) = start(context, Intent(Intent.ACTION_VIEW, address.toUri()))

/** Starts [intent]. A phone with nothing to take it leaves the page as it is: the address is shown on it. */
private fun start(context: Context, intent: Intent) {
    try {
        context.startActivity(intent)
    } catch (e: ActivityNotFoundException) {
        // Nothing more to do: the page shows the address, to be typed elsewhere.
    }
}

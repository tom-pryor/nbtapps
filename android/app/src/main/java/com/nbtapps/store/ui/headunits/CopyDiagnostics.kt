// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext

/** What the button, or the menu's row, says that copies the diagnostics of the app's Bluetooth links. */
internal const val COPY_DIAGNOSTICS = "Copy diagnostics"

/**
 * Puts [text], the diagnostics of the app's Bluetooth links ([com.nbtapps.store.data.link.SerialDiagnostics.report]),
 * on the clipboard as plain text, for the user to paste into a message to whoever helps them.
 */
internal fun copyDiagnostics(context: Context, text: String) {
    context.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText("NBTApps diagnostics", text))
}

/** "Copy diagnostics", where a connect failed: copies what [text] gives when it is tapped. */
@Composable
internal fun CopyDiagnosticsButton(text: () -> String) {
    val context = LocalContext.current
    TextButton(onClick = { copyDiagnostics(context, text()) }) { Text(COPY_DIAGNOSTICS) }
}

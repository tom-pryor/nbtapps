// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable

/**
 * The app's one theme: Midnight & Amber, dark whatever the phone's setting. It does not take the wallpaper's
 * dynamic colours, so the app looks the same on every phone and matches nbtapps.com.
 */
@Composable
fun NbtAppTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = NbtDarkColors, content = content)
}

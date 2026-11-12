// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.theme

import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.Color

/*
 * Midnight & Amber, the one theme of the app and of nbtapps.com. There is no light theme: the app is used in a
 * car, often at night. The site's tokens (src/styles/global.css in the site repo) carry the same values under
 * the same roles; a change here belongs there too.
 */

/** The page behind everything; also the window and launch-screen background (`nbt_background`). */
val Midnight = Color(0xFF0A111E)
val MidnightSurface = Color(0xFF111A2B)
val MidnightSurfaceHigh = Color(0xFF1A2539)
val MidnightSurfaceHighest = Color(0xFF212D44)
val Ink = Color(0xFFE6ECF5)
val InkMuted = Color(0xFF9AA8BF)

/** The launcher icon's background. */
val Navy = Color(0xFF1C3D6E)
val SkyBlue = Color(0xFF8CB2FF)
val Amber = Color(0xFFFFC14D)

/**
 * Every role is set, so none falls back to Material's baseline purples. Text and controls meet WCAG AA against
 * the surfaces they sit on. `outline` is lighter than the site's border colour because here it draws controls
 * (text fields, outlined buttons, radio buttons), which need 3:1 on every surface; the site's border colour is
 * `outlineVariant`, for dividers.
 */
val NbtDarkColors = darkColorScheme(
    primary = SkyBlue,
    onPrimary = Color(0xFF0A2152),
    primaryContainer = Navy,
    onPrimaryContainer = Color(0xFFDCE6FA),
    inversePrimary = Color(0xFF1F4FA8),

    secondary = Color(0xFFB6C4DD),
    onSecondary = Color(0xFF1F2B40),
    secondaryContainer = Color(0xFF2A3850),
    onSecondaryContainer = Color(0xFFDCE6FA),

    // Amber is for badges and highlights, always as a fill behind onTertiary.
    tertiary = Amber,
    onTertiary = Color(0xFF2A1A00),
    tertiaryContainer = Color(0xFF5C4200),
    onTertiaryContainer = Color(0xFFFFDEA6),

    background = Midnight,
    onBackground = Ink,
    surface = Midnight,
    onSurface = Ink,
    surfaceVariant = MidnightSurfaceHigh,
    onSurfaceVariant = InkMuted,
    surfaceTint = SkyBlue,
    surfaceDim = Midnight,
    surfaceBright = Color(0xFF27334A),
    surfaceContainerLowest = Color(0xFF070C16),
    surfaceContainerLow = Color(0xFF0E1626),
    surfaceContainer = MidnightSurface,
    surfaceContainerHigh = MidnightSurfaceHigh,
    surfaceContainerHighest = MidnightSurfaceHighest,
    inverseSurface = Ink,
    inverseOnSurface = MidnightSurface,

    outline = Color(0xFF6B7A94),
    outlineVariant = Color(0xFF2A3850),

    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6),
)

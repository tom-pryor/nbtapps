// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.menu

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DrawerState
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.NavigationDrawerItem
import androidx.compose.material3.NavigationDrawerItemDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R

/** Material's own sheet goes to 360 dp, which is the whole width of a small phone: nothing is left beside it to tap. */
private val MENU_WIDTH = 300.dp

/**
 * Whether any of the menu is in sight, or is about to be: [current] is where it last came to rest, and [target] where
 * it is going. So it shows from the tap that opens it until it has gone.
 */
internal fun menuShows(current: DrawerValue, target: DrawerValue): Boolean =
    current == DrawerValue.Open || target == DrawerValue.Open

/**
 * Whether the menu has to close for what is on top: a screen that is not one of the tabs, the menu being theirs. While
 * the screen on top is not known ([screenKnown] is false), as just after a rotation, it stays as it is.
 */
internal fun menuGivesWay(screenKnown: Boolean, tabOnTop: Boolean): Boolean = screenKnown && !tabOnTop

/** The menu button of a tab's top bar. */
@Composable
fun MenuButton(onClick: () -> Unit) {
    IconButton(onClick = onClick) {
        Icon(painterResource(R.drawable.ic_menu), contentDescription = "Open menu")
    }
}

/** The menu at the side: the app's name and [versionName], and About ([onOpenAbout]). */
@Composable
fun SideMenu(drawerState: DrawerState, versionName: String, onOpenAbout: () -> Unit) {
    ModalDrawerSheet(drawerState, Modifier.width(MENU_WIDTH)) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(vertical = 12.dp)) {
            Column(Modifier.padding(start = 28.dp, top = 8.dp, end = 28.dp, bottom = 16.dp)) {
                Text(
                    stringResource(R.string.app_name),
                    Modifier.semantics { heading() },
                    style = MaterialTheme.typography.titleLarge,
                )
                Text(
                    "Version $versionName",
                    Modifier.padding(top = 2.dp),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
            HorizontalDivider(Modifier.padding(start = 28.dp, end = 28.dp, bottom = 8.dp))
            NavigationDrawerItem(
                label = { Text("About", style = MaterialTheme.typography.labelLarge) },
                selected = false,
                onClick = onOpenAbout,
                modifier = Modifier.padding(NavigationDrawerItemDefaults.ItemPadding),
                icon = { Icon(painterResource(R.drawable.ic_info), contentDescription = null) },
                colors = NavigationDrawerItemDefaults.colors(unselectedTextColor = MaterialTheme.colorScheme.onSurface),
            )
        }
    }
}

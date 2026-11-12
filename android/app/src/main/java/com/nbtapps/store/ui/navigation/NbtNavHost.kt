// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.navigation

import androidx.annotation.DrawableRes
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.navigation.NavDestination.Companion.hasRoute
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.nbtapps.store.R
import com.nbtapps.store.ui.catalogue.CataloguePackageScreen
import com.nbtapps.store.ui.catalogue.CatalogueScreen
import com.nbtapps.store.ui.detail.HeadUnitDetailScreen
import com.nbtapps.store.ui.headunits.HeadUnitsScreen
import com.nbtapps.store.ui.job.JobScreen
import com.nbtapps.store.ui.pairing.PairingScreen
import com.nbtapps.store.ui.spptest.SppTestScreen
import kotlinx.serialization.Serializable

@Serializable
object HeadUnitsRoute

@Serializable
data class HeadUnitDetailRoute(val headUnitId: String)

/** Where this phone is paired with the head unit [headUnitId], by the code from the car's screen. */
@Serializable
data class PairingRoute(val headUnitId: String)

@Serializable
object SppTestRoute

@Serializable
object CatalogueRoute

@Serializable
data class CataloguePackageRoute(val packageId: String)

/**
 * One job on the connected head unit: installing [installId] at [installVersion], or removing [removeId].
 * With neither, the screen shows the job that is running or ran last.
 */
@Serializable
data class JobRoute(
    val installId: String? = null,
    val installVersion: String? = null,
    val removeId: String? = null,
)

/** A screen reachable from the bottom bar. */
private data class TopLevelDestination(val route: Any, val label: String, @DrawableRes val icon: Int)

private val topLevelDestinations = listOf(
    TopLevelDestination(CatalogueRoute, "Catalogue", R.drawable.ic_catalogue),
    TopLevelDestination(HeadUnitsRoute, "Head units", R.drawable.ic_head_unit),
)

/**
 * The app's screens. It opens on the catalogue, or when [startOnHeadUnits], on the head units list; the value must
 * not change while the app runs, since the screens are stacked on the one it opened on.
 */
@Composable
fun NbtNavHost(startOnHeadUnits: Boolean, navController: NavHostController = rememberNavController()) {
    val backStackEntry by navController.currentBackStackEntryAsState()
    val currentDestination = backStackEntry?.destination
    val currentTopLevel = topLevelDestinations.find { currentDestination?.hasRoute(it.route::class) == true }

    /** Goes to a screen of the bottom bar, keeping the stack of the one that is left for a return to it. */
    fun openTopLevel(route: Any) = navController.navigate(route) {
        popUpTo(navController.graph.findStartDestination().id) { saveState = true }
        launchSingleTop = true
        restoreState = true
    }
    // The head unit named in a top bar: its own screen, or with none saved, the list where one is added.
    val openHeadUnit = { id: String -> navController.navigate(HeadUnitDetailRoute(id)) }
    val addHeadUnit = { openTopLevel(HeadUnitsRoute) }
    val pairHeadUnit = { id: String -> navController.navigate(PairingRoute(id)) }

    Scaffold(
        bottomBar = {
            // Only the top-level screens show the bar; screens opened from them use the full height.
            if (currentTopLevel != null) {
                NavigationBar {
                    topLevelDestinations.forEach { destination ->
                        NavigationBarItem(
                            selected = destination == currentTopLevel,
                            onClick = { openTopLevel(destination.route) },
                            icon = { Icon(painterResource(destination.icon), contentDescription = null) },
                            label = { Text(destination.label) },
                        )
                    }
                }
            }
        },
        // Each screen has its own Scaffold that handles the system bars; this one only makes room for the bar.
        contentWindowInsets = WindowInsets(),
    ) { padding ->
        NavHost(
            navController = navController,
            startDestination = if (startOnHeadUnits) HeadUnitsRoute else CatalogueRoute,
            modifier = Modifier.padding(padding).consumeWindowInsets(padding),
        ) {
            composable<HeadUnitsRoute> {
                HeadUnitsScreen(
                    onOpenHeadUnit = { id -> navController.navigate(HeadUnitDetailRoute(id)) },
                    onOpenSppTest = { navController.navigate(SppTestRoute) },
                )
            }
            composable<HeadUnitDetailRoute> {
                HeadUnitDetailScreen(
                    onBack = { navController.popBackStack() },
                    onUninstall = { id -> navController.navigate(JobRoute(removeId = id)) },
                    onOpenJob = { navController.navigate(JobRoute()) },
                    onPair = pairHeadUnit,
                    onOpenPackage = { id -> navController.navigate(CataloguePackageRoute(id)) },
                )
            }
            composable<PairingRoute> {
                PairingScreen(onBack = { navController.popBackStack() })
            }
            composable<JobRoute> {
                JobScreen(
                    onBack = { navController.popBackStack() },
                    // In place of the job that offered it: that one is over, and this screen shows the newest job.
                    onInstall = { id, version ->
                        navController.navigate(JobRoute(installId = id, installVersion = version)) {
                            popUpTo<JobRoute> { inclusive = true }
                        }
                    },
                )
            }
            composable<SppTestRoute> {
                SppTestScreen(onBack = { navController.popBackStack() })
            }
            composable<CatalogueRoute> {
                CatalogueScreen(
                    onOpenPackage = { id -> navController.navigate(CataloguePackageRoute(id)) },
                    onOpenHeadUnit = openHeadUnit,
                    onAddHeadUnit = addHeadUnit,
                    onPairHeadUnit = pairHeadUnit,
                )
            }
            composable<CataloguePackageRoute> {
                CataloguePackageScreen(
                    onBack = { navController.popBackStack() },
                    onOpenPackage = { id -> navController.navigate(CataloguePackageRoute(id)) },
                    onInstall = { id, version -> navController.navigate(JobRoute(installId = id, installVersion = version)) },
                    onOpenHeadUnit = openHeadUnit,
                    onAddHeadUnit = addHeadUnit,
                    onPairHeadUnit = pairHeadUnit,
                )
            }
        }
    }
}

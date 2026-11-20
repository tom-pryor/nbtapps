// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.navigation

import androidx.annotation.DrawableRes
import androidx.compose.animation.EnterTransition
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.navigation.NavDestination.Companion.hasRoute
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.toRoute
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.ui.catalogue.CataloguePackageScreen
import com.nbtapps.store.ui.catalogue.CatalogueScreen
import com.nbtapps.store.ui.detail.HeadUnitDetailScreen
import com.nbtapps.store.ui.headunits.HeadUnitsScreen
import com.nbtapps.store.ui.job.JobScreen
import com.nbtapps.store.ui.logs.LogPullScreen
import com.nbtapps.store.ui.pairing.PairingScreen
import com.nbtapps.store.ui.settings.SettingsScreen
import com.nbtapps.store.ui.setup.SetupScreen
import com.nbtapps.store.ui.spptest.SppTestScreen
import com.nbtapps.store.ui.terminal.TerminalScreen
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.serialization.Serializable

@Serializable
object HeadUnitsRoute

@Serializable
data class HeadUnitDetailRoute(val headUnitId: String)

/** Where this phone is paired with the head unit [headUnitId], by the code from the car's screen. */
@Serializable
data class PairingRoute(val headUnitId: String)

/** The log files of the package [packageId] on the head unit [headUnitId], pulled into a ZIP to share. */
@Serializable
data class LogPullRoute(val headUnitId: String, val packageId: String)

/** The settings of the installed package [packageId] on the head unit [headUnitId], read and changed over the link. */
@Serializable
data class SettingsRoute(val headUnitId: String, val packageId: String)

/** The terminal on the head unit [headUnitId]: a shell through the SSH package, held by the app when the screen is left. */
@Serializable
data class TerminalRoute(val headUnitId: String)

@Serializable
object SppTestRoute

/**
 * The set-up that walks the user through adding the car, connecting, pairing and starting with the car: see
 * [SetupScreen]. [furtherCar]: it adds a car where there is one already, and begins at picking it, with no question
 * before it. The name of the property is the key [SetupViewModel][com.nbtapps.store.ui.setup.SetupViewModel] reads.
 */
@Serializable
data class SetupRoute(val furtherCar: Boolean)

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
 * The app's screens. It opens on the catalogue; with [openSetupAtStart], the set-up is opened over it at once, and
 * [onSetupOpened] told, so that the caller can keep it from being opened again by a rotation or a restored process.
 * Leaving the set-up goes back to the catalogue.
 *
 * [headUnitToOpen], when it is not null, is the head unit whose page another screen of the app asked to be shown, as
 * the screen for a handed-over pairing code does: the page is opened, and [onHeadUnitOpened] told. [terminalToOpen] is
 * likewise the head unit whose terminal the notification asked to be shown, and [onTerminalOpened] is told.
 */
@Composable
fun NbtNavHost(
    openSetupAtStart: Boolean,
    onSetupOpened: () -> Unit,
    navController: NavHostController = rememberNavController(),
    headUnitToOpen: String? = null,
    onHeadUnitOpened: () -> Unit = {},
    terminalToOpen: String? = null,
    onTerminalOpened: () -> Unit = {},
) {
    val backStackEntry by navController.currentBackStackEntryAsState()
    val currentDestination = backStackEntry?.destination
    val currentTopLevel = topLevelDestinations.find { currentDestination?.hasRoute(it.route::class) == true }

    /** Goes to a screen of the bottom bar, keeping the stack of the one that is left for a return to it. */
    fun openTopLevel(route: Any) = navController.navigate(route) {
        popUpTo(navController.graph.findStartDestination().id) { saveState = true }
        launchSingleTop = true
        restoreState = true
    }
    // The head unit named in a top bar: its own screen, or with none saved, the set-up, where one is added.
    val openHeadUnit = { id: String -> navController.navigate(HeadUnitDetailRoute(id)) }
    val setUp = { navController.navigate(SetupRoute(furtherCar = false)) }
    val addCar = { navController.navigate(SetupRoute(furtherCar = true)) }
    // Only the set-up itself is popped: a second tap while it goes must not pop the screen under it.
    val leaveSetup = {
        if (navController.currentBackStackEntry?.destination?.hasRoute(SetupRoute::class) == true) navController.popBackStack()
    }
    // The set-up opened at launch comes up at once, over a catalogue the user has not seen yet.
    val setupAtLaunch = remember { AtomicBoolean(false) }
    val pairHeadUnit = { id: String -> navController.navigate(PairingRoute(id)) }
    // From the pairing screen: back to the head unit's page where it was opened from there, or to the page in its place.
    val headUnitAfterPairing = { id: String ->
        navController.popBackStack()
        val below = navController.currentBackStackEntry?.takeIf { it.destination.hasRoute(HeadUnitDetailRoute::class) }
        if (below?.toRoute<HeadUnitDetailRoute>()?.headUnitId != id) navController.navigate(HeadUnitDetailRoute(id))
    }
    LaunchedEffect(openSetupAtStart) {
        if (openSetupAtStart) {
            setupAtLaunch.set(true)
            setUp()
            onSetupOpened()
        }
    }
    LaunchedEffect(headUnitToOpen) {
        if (headUnitToOpen != null) {
            val top = navController.currentBackStackEntry?.takeIf { it.destination.hasRoute(HeadUnitDetailRoute::class) }
            if (top?.toRoute<HeadUnitDetailRoute>()?.headUnitId != headUnitToOpen) navController.navigate(HeadUnitDetailRoute(headUnitToOpen))
            onHeadUnitOpened()
        }
    }
    LaunchedEffect(terminalToOpen) {
        if (terminalToOpen != null) {
            val top = navController.currentBackStackEntry?.takeIf { it.destination.hasRoute(TerminalRoute::class) }
            if (top?.toRoute<TerminalRoute>()?.headUnitId != terminalToOpen) navController.navigate(TerminalRoute(terminalToOpen))
            onTerminalOpened()
        }
    }

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
            startDestination = CatalogueRoute,
            modifier = Modifier.padding(padding).consumeWindowInsets(padding),
        ) {
            composable<HeadUnitsRoute> {
                HeadUnitsScreen(
                    onOpenHeadUnit = { id -> navController.navigate(HeadUnitDetailRoute(id)) },
                    onOpenSppTest = { navController.navigate(SppTestRoute) },
                    onSetUp = setUp,
                    onAddCar = addCar,
                )
            }
            composable<SetupRoute>(enterTransition = { if (setupAtLaunch.getAndSet(false)) EnterTransition.None else null }) {
                SetupScreen(
                    onLeave = leaveSetup,
                    onBrowse = {
                        leaveSetup()
                        openTopLevel(CatalogueRoute)
                    },
                    onOpenHeadUnit = { id ->
                        leaveSetup()
                        navController.navigate(HeadUnitDetailRoute(id))
                    },
                )
            }
            composable<HeadUnitDetailRoute> { entry ->
                val headUnitId = entry.toRoute<HeadUnitDetailRoute>().headUnitId
                HeadUnitDetailScreen(
                    onBack = { navController.popBackStack() },
                    onUninstall = { id -> navController.navigate(JobRoute(removeId = id)) },
                    onOpenJob = { navController.navigate(JobRoute()) },
                    onPair = pairHeadUnit,
                    onOpenPackage = { id -> navController.navigate(CataloguePackageRoute(id)) },
                    onOpenLogs = { id -> navController.navigate(LogPullRoute(headUnitId, id)) },
                    onOpenSettings = { id -> navController.navigate(SettingsRoute(headUnitId, id)) },
                    onOpenTerminal = { id -> navController.navigate(TerminalRoute(id)) },
                )
            }
            composable<TerminalRoute> {
                TerminalScreen(
                    onBack = { navController.popBackStack() },
                    onOpenPackage = { id -> navController.navigate(CataloguePackageRoute(id)) },
                )
            }
            composable<LogPullRoute> {
                LogPullScreen(onBack = { navController.popBackStack() })
            }
            composable<SettingsRoute> {
                SettingsScreen(
                    onBack = { navController.popBackStack() },
                    onUpdateAgent = { navController.navigate(CataloguePackageRoute(AGENT_PACKAGE_ID)) },
                )
            }
            composable<PairingRoute> {
                PairingScreen(
                    onBack = { navController.popBackStack() },
                    onOpenJob = { navController.navigate(JobRoute()) },
                    onOpenHeadUnit = headUnitAfterPairing,
                )
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
                    onSetUp = setUp,
                    onPairHeadUnit = pairHeadUnit,
                )
            }
            composable<CataloguePackageRoute> {
                CataloguePackageScreen(
                    onBack = { navController.popBackStack() },
                    onOpenPackage = { id -> navController.navigate(CataloguePackageRoute(id)) },
                    onInstall = { id, version -> navController.navigate(JobRoute(installId = id, installVersion = version)) },
                    onOpenHeadUnit = openHeadUnit,
                    onSetUp = setUp,
                    onPairHeadUnit = pairHeadUnit,
                    onOpenSettings = { headUnitId, id -> navController.navigate(SettingsRoute(headUnitId, id)) },
                    onUninstall = { id -> navController.navigate(JobRoute(removeId = id)) },
                )
            }
        }
    }
}

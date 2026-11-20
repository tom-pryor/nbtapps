// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import android.text.format.Formatter
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.PrereleaseAsk
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.LATER_FEATURES_WARNING
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.ui.headunits.HeadUnitIndicator

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CataloguePackageScreen(
    onBack: () -> Unit,
    onOpenPackage: (String) -> Unit,
    onInstall: (packageId: String, version: String) -> Unit,
    onOpenHeadUnit: (String) -> Unit,
    onSetUp: () -> Unit,
    onPairHeadUnit: (String) -> Unit,
    onOpenSettings: (headUnitId: String, packageId: String) -> Unit,
    onUninstall: (packageId: String) -> Unit,
    viewModel: CataloguePackageViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val pkg = uiState.pkg
    // The package whose pre-releases the user is being asked to turn on: this one, or one this one needs. Kept
    // across a rotation, so that the question is still there.
    var asking by rememberSaveable(stateSaver = AskSaver) { mutableStateOf<PrereleaseAsk?>(null) }

    Scaffold(
        topBar = {
            TopAppBar(
                // One line: the head unit's name shares the bar with it.
                title = { Text(pkg?.name ?: "Package", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back")
                    }
                },
                actions = { HeadUnitIndicator(onOpenHeadUnit, onSetUp, onPairHeadUnit) },
            )
        },
    ) { padding ->
        when {
            uiState.loading -> Box(Modifier.padding(padding).fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
            pkg == null -> Box(Modifier.padding(padding).fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("This package isn't in the catalogue.")
            }
            else -> Column(
                Modifier
                    .padding(padding)
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                val self = PrereleaseAsk(pkg.id, pkg.name, uiState.newest?.version.orEmpty())
                Header(pkg)
                if (pkg.community) CommunityCard(pkg, uiState.sourceName)
                uiState.get?.let { get ->
                    GetCard(
                        get,
                        onDownload = viewModel::download,
                        onInstall = { onInstall(pkg.id, get.version.version) },
                        onAskSwitch = { asking = self },
                        onAsk = { asking = it },
                        onSettings = { get.headUnitId?.let { onOpenSettings(it, pkg.id) } },
                        onUninstall = { onUninstall(pkg.id) },
                    )
                }
                if (uiState.showsPrereleaseSwitch) {
                    PrereleaseCard(
                        agent = pkg.id == AGENT_PACKAGE_ID,
                        on = uiState.prereleaseOn,
                        // Turning it off takes nothing away: what is installed stays until a stable version above it.
                        onSet = { on -> if (on) asking = self else viewModel.setPrerelease(pkg.id, false) },
                    )
                }
                uiState.newest?.let { newest ->
                    LatestVersionCard(newest, prerelease = pkg.isPrerelease(newest.version))
                    if (newest.riskFlags.isNotEmpty() || newest.unknownFlags) RiskCard(newest)
                }
                if (uiState.dependencies.isNotEmpty()) DependenciesCard(uiState.dependencies, onOpenPackage)
                DetailsSections(uiState.details, pkg, onRetry = viewModel::retryDetails)
                val versions = allVersions(pkg)
                if (versions.size > 1) VersionsCard(versions)
                Text(
                    listOfNotNull(pkg.id, uiState.sourceName).joinToString("\n"),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }

    asking?.let { ask ->
        PrereleaseDialog(
            ask,
            onConfirm = {
                viewModel.setPrerelease(ask.id, true)
                asking = null
            },
            onDismiss = { asking = null },
        )
    }
}

/** A question that is open, as the screen's saved state keeps it: the package's id, name and version. */
private val AskSaver = Saver<PrereleaseAsk?, ArrayList<String>>(
    save = { ask -> ask?.let { arrayListOf(it.id, it.name, it.version) } },
    restore = { saved -> PrereleaseAsk(saved[0], saved[1], saved[2]) },
)

/** The title of the question put before pre-releases of [ask]'s package are turned on. */
internal fun prereleaseQuestion(ask: PrereleaseAsk): String = "Turn on pre-releases of ${ask.name}?"

/**
 * What the question says. The agent's own is the stronger: the agent takes no older version of itself, so a head
 * unit that has a pre-release agent cannot be taken back from the app.
 */
internal fun prereleaseWarning(ask: PrereleaseAsk): String =
    if (ask.id == AGENT_PACKAGE_ID) {
        "Pre-release versions of the agent are still being tested and may not work as well. A head unit that has " +
            "one can't be taken back to an older agent from the app: the only way back is to reinstall the agent " +
            "from a USB stick. This applies to every head unit."
    } else {
        "Pre-release versions are still being tested and may not work as well. The newest version is then offered, " +
            "pre-release or not, on every head unit. If you turn this off later, a head unit keeps what it has " +
            "until a newer stable version is out."
    }

/** What the switch's card says under the switch. */
internal fun prereleaseSwitchText(on: Boolean, agent: Boolean): String = when {
    on -> "On for every head unit: the newest version is offered, pre-release or not. Turned off, a head unit " +
        "keeps what it has until a newer stable version is out."
    agent -> "Versions of the agent that are still being tested. The only way back from one is to reinstall the " +
        "agent from a USB stick."
    else -> "Versions that are still being tested, before everyone gets them. They may not work as well."
}

@Composable
private fun PrereleaseDialog(ask: PrereleaseAsk, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(prereleaseQuestion(ask)) },
        text = { Text(prereleaseWarning(ask)) },
        confirmButton = { TextButton(onClick = onConfirm) { Text("Turn on") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** The package's pre-release switch: on the phone, for every head unit, until it is turned off. */
@Composable
private fun PrereleaseCard(agent: Boolean, on: Boolean, onSet: (Boolean) -> Unit) {
    Section("Pre-release versions") {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Offer pre-release versions", Modifier.weight(1f))
            Switch(checked = on, onCheckedChange = onSet)
        }
        Text(
            prereleaseSwitchText(on, agent),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun Header(pkg: CataloguePackage) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        PackageIcon(pkg.name, 56.dp)
        Column {
            Text(pkg.name, style = MaterialTheme.typography.headlineSmall)
            if (pkg.community) CommunityLabel("")
            if (pkg.prereleaseOnly) PrereleaseLabel(label = PRERELEASE_ONLY)
            Text(
                listOf(pkg.author, categoryLabel(pkg.category)).filter { it.isNotEmpty() }.joinToString(" · "),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
    if (pkg.summary.isNotEmpty()) Text(pkg.summary, style = MaterialTheme.typography.bodyLarge)
}

/** Who made a community package and what the catalogue vouches for: the file it reviewed, not the developer. */
@Composable
private fun CommunityCard(pkg: CataloguePackage, sourceName: String?) {
    val source = sourceName ?: "the catalogue"
    val maker = pkg.author.ifEmpty { "an outside developer" }
    Section("Community package") {
        Text("Made by $maker, not by $source.", style = MaterialTheme.typography.bodyMedium)
        Text(
            "${source.replaceFirstChar { it.uppercase() }} reviewed each version listed here and signed it. It downloads from the developer's own " +
                "release, and the phone and the head unit both check that it is the file that was signed.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * The way from the catalogue to the head unit: download to the phone at home, install in the car. A package out
 * only in pre-releases asks for its switch first ([onAskSwitch]), and a version that needs another package's
 * pre-release asks for that one's ([onAsk]). An installed package with settings has Settings at the right of the
 * card's title, which needs a link to that head unit ([onSettings]). An installed package but the agent has
 * Uninstall, which opens the job that removes it ([onUninstall]).
 */
@Composable
private fun GetCard(
    get: GetState,
    onDownload: () -> Unit,
    onInstall: () -> Unit,
    onAskSwitch: () -> Unit,
    onAsk: (PrereleaseAsk) -> Unit,
    onSettings: () -> Unit,
    onUninstall: () -> Unit,
) {
    val settings: (@Composable () -> Unit)? = if (get.settings) {
        { TextButton(onClick = onSettings, enabled = get.connected) { Text("Settings") } }
    } else {
        null
    }
    Section(if (get.installedVersion != null) "Installed: ${get.installedVersion}" else "Get it", trailing = settings) {
        get.installedAsOf?.let { seenAt ->
            Text(
                "On ${get.headUnitName} ${asOf(seenAt)}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (get.installedPrerelease) PrereleaseLabel(detail = "The installed version")
        if (get.needsSwitch) {
            if (get.installedVersion != null) {
                Text("${get.headUnitName} has a pre-release of it. There is no stable version yet.")
            } else {
                Text(
                    "Only pre-release versions of this package are out so far. To install it, turn on pre-release versions.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Button(onClick = onAskSwitch) { Text("Install") }
            }
        } else {
            Offer(get, onDownload, onInstall, onAsk)
        }
        // An installed package can be taken off whatever is on offer, so this comes after either.
        if (get.uninstall.showsButton) {
            OutlinedButton(onClick = onUninstall, enabled = get.uninstall == UninstallState.Ready) { Text("Uninstall") }
        }
        listOfNotNull(uninstallLine(get), jobLine(get)).forEach { line ->
            Text(line, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/**
 * The part of [GetCard] for a package whose version on offer the user may download and install: the download, what
 * stands in the way of the install, and the Install or Update button.
 */
@Composable
private fun ColumnScope.Offer(get: GetState, onDownload: () -> Unit, onInstall: () -> Unit, onAsk: (PrereleaseAsk) -> Unit) {
    val context = LocalContext.current
    if (get.offeredPrerelease && !get.upToDate) PrereleaseLabel(detail = "Version ${get.version.version}")
    val downloading = get.downloading
    when {
        downloading != null -> {
            Text("Downloading…")
            LinearProgressIndicator(
                progress = { if (downloading.total > 0) downloading.bytes.toFloat() / downloading.total else 0f },
                modifier = Modifier.fillMaxWidth(),
            )
        }
        get.toDownload.isNotEmpty() -> {
            val size = Formatter.formatShortFileSize(context, get.toDownload.sumOf { it.version.size })
            val others = get.toDownload.filter { it.version.sha256 != get.version.sha256 }
            if (others.isNotEmpty()) {
                Text("Also needs ${others.joinToString { "${it.pkg.name} ${it.version.version}" }}.", style = MaterialTheme.typography.bodyMedium)
            }
            get.downloadError?.let { Text("The download failed: $it", color = MaterialTheme.colorScheme.error) }
            Button(onClick = onDownload) { Text(if (get.stored) "Download what it needs ($size)" else "Download ${get.version.version} ($size)") }
        }
        else -> Text("Version ${get.version.version} is on this phone, checked and ready to install.")
    }
    if (get.laterFormat) LaterFeatures()
    get.needsNewerApp?.let { newer ->
        Text("Version $newer ${NEEDS_NEWER_APP.lowercase()}.", style = MaterialTheme.typography.bodyMedium)
    }
    get.asks.forEach { ask ->
        Text("Needs ${ask.name} ${ask.version}, which is a pre-release.", style = MaterialTheme.typography.bodyMedium)
        OutlinedButton(onClick = { onAsk(ask) }) { Text("Turn on pre-releases of ${ask.name}") }
    }
    val connectLine = connectLine(get)
    when {
        get.incompatible != null -> Text(get.incompatible, color = MaterialTheme.colorScheme.error)
        get.upToDate -> Text("${get.headUnitName} has this version or a newer one.")
        connectLine != null -> Text(
            connectLine,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        else -> Button(onClick = onInstall, enabled = get.canInstall) {
            Text(if (get.installedVersion != null) "Update on ${get.headUnitName}" else "Install on ${get.headUnitName}")
        }
    }
}

/** The Uninstall button is on the card, pressed or greyed: for any package that is installed but the agent. */
internal val UninstallState.showsButton: Boolean
    get() = this == UninstallState.Ready ||
        (this is UninstallState.Blocked && reason != UninstallState.Reason.AGENT)

/**
 * The line that says to connect, in place of Install or Update, where the version on offer could go on the head unit
 * but there is no link to it. For an installed package it says that Uninstall needs the link too. Null where the
 * card says something else there.
 */
internal fun connectLine(get: GetState): String? {
    if (get.needsSwitch || get.incompatible != null || get.upToDate || get.connected) return null
    val what = when {
        get.uninstall.showsButton -> "update or uninstall"
        get.installedVersion != null -> "update"
        else -> "install"
    }
    return "Connect to ${get.headUnitName ?: "a head unit"} to $what it, from its name at the top of this screen. " +
        "The download needs no car."
}

/**
 * The line under Uninstall: why the agent has none, or that it needs a link. Null where there is nothing to say, or
 * where [connectLine] says it already, or where it waits for a job ([jobLine] says so).
 */
internal fun uninstallLine(get: GetState): String? {
    val blocked = get.uninstall as? UninstallState.Blocked ?: return null
    return when (blocked.reason) {
        UninstallState.Reason.AGENT -> AGENT_NOT_UNINSTALLED
        UninstallState.Reason.NO_LINK -> if (connectLine(get) != null) {
            null
        } else {
            "Connect to ${get.headUnitName ?: "a head unit"} to uninstall it, from its name at the top of this screen."
        }
        UninstallState.Reason.BUSY -> null
    }
}

internal const val AGENT_NOT_UNINSTALLED = "The agent can't be uninstalled. Everything else on the head unit needs it."

/**
 * Why buttons that could otherwise be pressed are greyed while a job runs: one line for all of them, naming those
 * it greys. Null when no job greys any. Install or Update counts only where nothing else stands in its way.
 */
internal fun jobLine(get: GetState): String? {
    val install = get.busy && get.readyToInstall
    val uninstall = get.uninstall == UninstallState.Blocked(UninstallState.Reason.BUSY)
    val what = when {
        install && uninstall -> "Update or uninstall"
        install -> if (get.installedVersion != null) "Update" else "Install"
        uninstall -> "Uninstall"
        else -> return null
    }
    return "A job is running on a head unit. $what once it has finished."
}

@Composable
private fun LatestVersionCard(version: PackageVersion, prerelease: Boolean) {
    Section("Version ${version.version}") {
        if (prerelease) PrereleaseLabel()
        Text(versionFacts(version))
        version.agentRange?.let { Text("Needs agent ${rangeLabel(it)}") }
        version.requires.app?.let { Text("Needs app ${rangeLabel(it)}") }
    }
}

@Composable
private fun RiskCard(version: PackageVersion) {
    Section("Before you install") {
        version.riskFlags.forEach { flag ->
            Column {
                Text(flag.label, style = MaterialTheme.typography.titleSmall)
                Text(flag.explanation, style = MaterialTheme.typography.bodyMedium)
            }
        }
        // A flag the catalogue publishes that this app cannot explain is never left out unsaid.
        if (version.unknownFlags) LaterFeatures()
    }
}

/** That the package uses what this version of the app does not know: see [LATER_FEATURES_WARNING]. */
@Composable
private fun LaterFeatures() {
    Text(LATER_FEATURES_WARNING, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.tertiary)
}

@Composable
private fun DependenciesCard(dependencies: List<DependencyRow>, onOpenPackage: (String) -> Unit) {
    Section("Depends on") {
        dependencies.forEach { dependency ->
            val label = listOfNotNull(dependency.name, dependency.versionRange?.let(::rangeLabel)).joinToString(" ")
            if (dependency.inCatalogue) {
                Text(
                    label,
                    Modifier.fillMaxWidth().clickable { onOpenPackage(dependency.id) }.padding(vertical = 4.dp),
                    color = MaterialTheme.colorScheme.primary,
                )
            } else {
                Text("$label (not in this catalogue)", Modifier.padding(vertical = 4.dp))
            }
        }
    }
}

@Composable
private fun DetailsSections(state: DetailsState, pkg: CataloguePackage, onRetry: () -> Unit) {
    when (state) {
        DetailsState.Loading -> Section("About") { CircularProgressIndicator() }
        is DetailsState.Failed -> Section("About") {
            Text("Couldn't load the description: ${state.reason}", color = MaterialTheme.colorScheme.error)
            TextButton(onClick = onRetry) { Text("Try again") }
        }
        is DetailsState.Loaded -> {
            val details = state.details
            if (details.description.isNotBlank()) Section("About") { Text(details.description) }
            val rows = changelog(details, pkg)
            if (rows.isNotEmpty()) ChangelogCard(rows)
        }
    }
}

@Composable
private fun ChangelogCard(rows: List<ChangelogRow>) {
    Section("What's new") {
        rows.forEach { row ->
            Column {
                VersionTitle(row.entry.version, row.prerelease)
                row.entry.notes.forEach { note -> Text("• $note", style = MaterialTheme.typography.bodyMedium) }
            }
        }
    }
}

@Composable
private fun VersionsCard(versions: List<VersionRow>) {
    Section("All versions") {
        versions.forEach { row ->
            Column {
                VersionTitle(row.version.version, row.prerelease)
                Text(versionFacts(row.version), style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

/** A version as a list's heading, with the pre-release label beside it where it is one. */
@Composable
private fun VersionTitle(version: String, prerelease: Boolean) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(version, style = MaterialTheme.typography.titleSmall)
        if (prerelease) PrereleaseLabel()
    }
}

/** A card with a title, and with [trailing] at the right of the title's row where it is given. */
@Composable
private fun Section(title: String, trailing: (@Composable () -> Unit)? = null, content: @Composable ColumnScope.() -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if (trailing == null) {
                Text(title, style = MaterialTheme.typography.titleMedium)
            } else {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(title, Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
                    trailing()
                }
            }
            content()
        }
    }
}

/** Release date and download size on one line, such as "Released 2026-11-01 · 48 kB". */
@Composable
private fun versionFacts(version: PackageVersion): String {
    val size = Formatter.formatShortFileSize(LocalContext.current, version.size)
    return listOfNotNull(version.released?.let { "Released $it" }, size).joinToString(" · ")
}

/** What a version that only a newer version of this app may install says for itself ([AppVersion][com.nbtapps.store.model.AppVersion]). */
internal const val NEEDS_NEWER_APP = "Needs a newer version of the app"

/** Makes a semver range readable: ">=2.0.0 <3.0.0" becomes "2.0.0 or later, below 3.0.0". */
internal fun rangeLabel(range: String): String =
    range.trim().split(Regex("\\s+")).joinToString(", ") { part ->
        when {
            part.startsWith(">=") -> "${part.drop(2)} or later"
            part.startsWith("<=") -> "${part.drop(2)} or earlier"
            part.startsWith(">") -> "above ${part.drop(1)}"
            part.startsWith("<") -> "below ${part.drop(1)}"
            part.startsWith("=") -> part.drop(1)
            else -> part
        }
    }

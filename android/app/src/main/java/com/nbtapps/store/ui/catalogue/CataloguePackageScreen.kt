// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import android.text.format.Formatter
import androidx.annotation.DrawableRes
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
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
import com.nbtapps.store.ui.setup.openWebPage

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
                val installed = uiState.get?.installedVersion
                // The lists point to the website for what they leave out, where there is one.
                val website = (uiState.details as? DetailsState.Loaded)?.details?.homepage?.let(::website)
                Header(pkg, headerMarks(uiState.get))
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
                DetailsSections(uiState.details, pkg, installed, website, onRetry = viewModel::retryDetails)
                val versions = allVersions(pkg, installed)
                if (versions.size > 1) VersionsCard(versions, website != null, uiState.newest)
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

/** The package's name and who made it, and under them, as the list's row marks it, its copy on the head unit. */
@Composable
private fun Header(pkg: CataloguePackage, marks: HeaderMarks) {
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
            if (marks.installed != null || marks.update != null) {
                FlowRow(
                    Modifier.padding(top = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                    itemVerticalAlignment = Alignment.CenterVertically,
                ) {
                    marks.installed?.let { InstalledMark(it) }
                    marks.update?.let { UpdateMark(it) }
                }
            }
        }
    }
    if (pkg.summary.isNotEmpty()) Text(pkg.summary, style = MaterialTheme.typography.bodyLarge)
}

/**
 * Who made a community package and what the catalogue vouches for: the file it reviewed, not the developer. That
 * second part is behind the info icon beside the title, so that it does not stand in the way each time.
 */
@Composable
private fun CommunityCard(pkg: CataloguePackage, sourceName: String?) {
    val source = sourceName ?: "the catalogue"
    val maker = pkg.author.ifEmpty { "an outside developer" }
    var open by rememberSaveable { mutableStateOf(false) }
    Card(Modifier.fillMaxWidth()) {
        // The title's row is the button, and taller than a title: the card's padding above it is less by as much.
        Column(Modifier.padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp)) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 40.dp)
                    .clickable(role = Role.Button, onClickLabel = if (open) "Close" else "Open") { open = !open }
                    .semantics { stateDescription = if (open) "Open" else "Closed" },
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("Community package", style = MaterialTheme.typography.titleMedium)
                Icon(
                    painterResource(R.drawable.ic_info),
                    contentDescription = "About community packages",
                    Modifier.size(20.dp),
                    tint = if (open) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Made by $maker, not by $source.", style = MaterialTheme.typography.bodyMedium)
                if (open) {
                    Text(
                        "${source.replaceFirstChar { it.uppercase() }} reviewed each version listed here and signed it. It downloads from the developer's own " +
                            "release, and the phone and the head unit both check that it is the file that was signed.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
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
        {
            TextButton(onClick = onSettings, enabled = get.connected, contentPadding = ButtonDefaults.TextButtonWithIconContentPadding) {
                IconLabel(R.drawable.ic_settings, "Settings")
            }
        }
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
                Button(onClick = onAskSwitch, contentPadding = ButtonDefaults.ButtonWithIconContentPadding) {
                    IconLabel(R.drawable.ic_add, "Install")
                }
            }
        } else {
            Offer(get, onDownload, onInstall, onAsk)
        }
        // An installed package can be taken off whatever is on offer, so this comes after either.
        if (get.uninstall.showsButton) {
            OutlinedButton(
                onClick = onUninstall,
                enabled = get.uninstall == UninstallState.Ready,
                // In the primary colour while it may be pressed: the outlined button's own grey would look
                // greyed out beside the card's other buttons. Greyed, it keeps the default look.
                colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.primary),
                contentPadding = ButtonDefaults.ButtonWithIconContentPadding,
            ) { IconLabel(R.drawable.ic_delete, "Uninstall") }
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
        !get.showsDownload -> Unit
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
            Button(onClick = onDownload, contentPadding = ButtonDefaults.ButtonWithIconContentPadding) {
                IconLabel(R.drawable.ic_download, if (get.stored) "Download what it needs ($size)" else "Download ${get.version.version} ($size)")
            }
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
        else -> Button(onClick = onInstall, enabled = get.canInstall, contentPadding = ButtonDefaults.ButtonWithIconContentPadding) {
            if (get.installedVersion != null) {
                IconLabel(R.drawable.ic_upgrade, "Update on ${get.headUnitName}")
            } else {
                IconLabel(R.drawable.ic_add, "Install on ${get.headUnitName}")
            }
        }
    }
}

/** A button's icon and label, spaced as Material's buttons with an icon are. The icon only goes with the words. */
@Composable
private fun IconLabel(@DrawableRes icon: Int, label: String) {
    Icon(painterResource(icon), contentDescription = null, Modifier.size(ButtonDefaults.IconSize))
    Spacer(Modifier.width(ButtonDefaults.IconSpacing))
    Text(label)
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
    Section("Latest version: ${version.version}") {
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

/**
 * The cards made from the package's details: About, and what is new. [installed] is the version on the head unit,
 * which what is new marks, and [website] the package's own address, checked ([website]), where it has one.
 */
@Composable
private fun DetailsSections(state: DetailsState, pkg: CataloguePackage, installed: String?, website: String?, onRetry: () -> Unit) {
    when (state) {
        DetailsState.Loading -> Section("About") { CircularProgressIndicator() }
        is DetailsState.Failed -> Section("About") {
            Text("Couldn't load the description: ${state.reason}", color = MaterialTheme.colorScheme.error)
            TextButton(onClick = onRetry) { Text("Try again") }
        }
        is DetailsState.Loaded -> {
            val details = state.details
            if (details.description.isNotBlank() || website != null) AboutCard(details.description, website)
            val rows = changelog(details, pkg, installed)
            if (rows.isNotEmpty()) ChangelogCard(rows, website != null)
        }
    }
}

/**
 * The description, cut to [ABOUT_CLOSED_LINES] lines with Show more where it is longer than [ABOUT_LINES], and the
 * Website button where the package has one, on the same row, so that it is there with the text cut.
 */
@Composable
private fun AboutCard(description: String, website: String?) {
    val context = LocalContext.current
    var open by rememberSaveable { mutableStateOf(false) }
    // Whether the text is longer than ABOUT_LINES, which only laying it out tells: unknown until then.
    var long by remember(description) { mutableStateOf<Boolean?>(null) }
    Section("About") {
        if (description.isNotBlank()) {
            Text(
                description,
                maxLines = aboutMaxLines(open, long),
                overflow = TextOverflow.Ellipsis,
                onTextLayout = { layout ->
                    when {
                        open -> long = layout.lineCount > ABOUT_LINES
                        long == null -> long = layout.hasVisualOverflow
                    }
                },
            )
        }
        val more = long == true
        if (more || website != null) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                if (more) ShowMoreButton(if (open) "Show less" else "Show more", open) { open = !open }
                Spacer(Modifier.weight(1f))
                if (website != null) {
                    TextButton(
                        onClick = { openWebPage(context, website) },
                        contentPadding = ButtonDefaults.TextButtonWithIconContentPadding,
                    ) { IconLabel(R.drawable.ic_open_in_new, "Website") }
                }
            }
        }
    }
}

/** A description up to this many lines is shown whole. */
internal const val ABOUT_LINES = 8

/** A longer one is cut to this many until Show more. */
internal const val ABOUT_CLOSED_LINES = 5

/**
 * How many lines of the description are laid out: all once [open]; [ABOUT_CLOSED_LINES] once it is known to be
 * [long]; otherwise [ABOUT_LINES], which tells whether it is.
 */
internal fun aboutMaxLines(open: Boolean, long: Boolean?): Int = when {
    open -> Int.MAX_VALUE
    long == true -> ABOUT_CLOSED_LINES
    else -> ABOUT_LINES
}

/** The text button that opens or closes a card's longer part: [label], and an arrow that turns while [open]. */
@Composable
private fun ShowMoreButton(label: String, open: Boolean, onClick: () -> Unit) {
    TextButton(onClick = onClick) {
        Text(label)
        Spacer(Modifier.width(ButtonDefaults.IconSpacing))
        Icon(
            painterResource(R.drawable.ic_expand_more),
            contentDescription = null,
            Modifier.size(ButtonDefaults.IconSize).rotate(if (open) 180f else 0f),
        )
    }
}

/** A line under a list that says what it leaves out. */
@Composable
private fun ListNotice(text: String) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun ChangelogCard(rows: List<ChangelogRow>, website: Boolean) {
    var open by rememberSaveable { mutableStateOf(false) }
    val shown = shownChangelog(rows, open, website)
    Section("What's new") {
        shown.rows.forEach { row ->
            Column {
                VersionTitle(row.entry.version, row.prerelease, released = row.released, installed = row.installed)
                row.entry.notes.forEach { note -> Text("• $note", style = MaterialTheme.typography.bodyMedium) }
            }
        }
        shown.notice?.let { ListNotice(it) }
        shown.toggle?.let { ShowMoreButton(it, open) { open = !open } }
    }
}

/**
 * Every version, closed to one row: the installed one, or the newest on offer ([newest], the version the latest
 * version card names).
 */
@Composable
private fun VersionsCard(versions: List<VersionRow>, website: Boolean, newest: PackageVersion?) {
    var open by rememberSaveable { mutableStateOf(false) }
    val shown = shownVersions(versions, open, website, newest)
    Section("All versions") {
        shown.rows.forEach { VersionItem(it) }
        shown.extra?.let { row ->
            // The installed version, older than the rows above: set apart, as it is not next in the list.
            HorizontalDivider()
            VersionItem(row)
        }
        shown.notice?.let { ListNotice(it) }
        shown.toggle?.let { ShowMoreButton(it, open) { open = !open } }
    }
}

@Composable
private fun VersionItem(row: VersionRow) {
    Column {
        VersionTitle(row.version.version, row.prerelease, installed = row.installed)
        Text(versionFacts(row.version), style = MaterialTheme.typography.bodyMedium)
    }
}

/**
 * A version as a list's heading: with its release date where given, the pre-release label where it is one, and
 * the mark of the version on the head unit where it is that one.
 */
@Composable
private fun VersionTitle(version: String, prerelease: Boolean, released: String? = null, installed: Boolean = false) {
    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(2.dp),
        itemVerticalAlignment = Alignment.CenterVertically,
    ) {
        Text(version, style = MaterialTheme.typography.titleSmall)
        released?.let {
            Text("· $it", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (prerelease) PrereleaseLabel()
        if (installed) InstalledMark("Installed")
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

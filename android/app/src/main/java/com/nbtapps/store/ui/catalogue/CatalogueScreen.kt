// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import android.text.format.DateUtils
import android.text.format.Formatter
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Badge
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.AgentUpdate
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.PackageStatus
import com.nbtapps.store.data.update.AppUpdateOffer
import com.nbtapps.store.data.update.UpdateState
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.ui.headunits.HeadUnitIndicator

/**
 * The catalogue's packages, against the head unit the app is working with: which of them it has, in which
 * version, and which have an update, with those first. The top bar names that head unit ([HeadUnitIndicator]):
 * [onOpenHeadUnit] opens its screen, [onAddHeadUnit] goes to where one is added, and [onPairHeadUnit] to where
 * the phone is paired with it.
 */
@Composable
fun CatalogueScreen(
    onOpenPackage: (String) -> Unit,
    onOpenHeadUnit: (String) -> Unit,
    onAddHeadUnit: () -> Unit,
    onPairHeadUnit: (String) -> Unit,
    viewModel: CatalogueViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    CatalogueContent(
        uiState = uiState,
        headUnit = { HeadUnitIndicator(onOpenHeadUnit, onAddHeadUnit, onPairHeadUnit) },
        onQueryChange = viewModel::setQuery,
        onSelectFilter = viewModel::selectFilter,
        onRefresh = viewModel::refresh,
        onUpdateApp = viewModel::updateApp,
        onOpenPackage = onOpenPackage,
        onOpenHeadUnit = onOpenHeadUnit,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CatalogueContent(
    uiState: CatalogueUiState,
    headUnit: @Composable () -> Unit,
    onQueryChange: (String) -> Unit,
    onSelectFilter: (CatalogueFilter) -> Unit,
    onRefresh: () -> Unit,
    onUpdateApp: () -> Unit,
    onOpenPackage: (String) -> Unit,
    onOpenHeadUnit: (String) -> Unit,
) {
    val unit = uiState.headUnit
    val listState = rememberLazyListState()
    // The rows are keyed, so a list that gains rows above its first one would stay on that one and leave the new
    // ones out of sight above it: the updates and the agent's card, when the head unit's list arrives after the
    // catalogue, and the app's own update. A list that was at its top stays at its top; one the user has scrolled is left where it is.
    val atTop = !listState.canScrollBackward && !listState.isScrollInProgress
    SideEffect { if (atTop) listState.requestScrollToItem(0) }
    Scaffold(
        topBar = { TopAppBar(title = { Text("Catalogue") }, actions = { headUnit() }) },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (unit != null) {
                AgentText.restart(unit.restartNeeded)?.let { need ->
                    RestartBanner("$need what was installed on ${unit.name}.", onOpenHeadUnit = { onOpenHeadUnit(unit.headUnitId) })
                }
            }
            OutlinedTextField(
                value = uiState.query,
                onValueChange = onQueryChange,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                placeholder = { Text("Search packages") },
                singleLine = true,
            )
            if (uiState.categories.isNotEmpty() || unit != null) {
                FilterRow(uiState, onSelectFilter)
            }
            if (unit != null && !unit.live && uiState.loaded) SavedListLine(unit)
            if (uiState.refreshingHeadUnit && unit != null) {
                Text(
                    "Refreshing the catalogue and ${unit.name}…",
                    Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            PullToRefreshBox(
                // The first load has its own spinner in the list.
                isRefreshing = uiState.refreshing && uiState.loaded,
                onRefresh = onRefresh,
                modifier = Modifier.fillMaxSize(),
            ) {
                // Always a scrollable list, so pull-to-refresh also works on the empty and error states.
                LazyColumn(Modifier.fillMaxSize(), state = listState) {
                    val message = uiState.emptyMessage()
                    when {
                        !uiState.loaded && uiState.error == null -> item {
                            Box(Modifier.fillParentMaxSize(), contentAlignment = Alignment.Center) {
                                CircularProgressIndicator()
                            }
                        }
                        message != null -> item {
                            Message(message, Modifier.fillParentMaxSize(), onRetry = onRefresh.takeIf { !uiState.loaded })
                        }
                        else -> {
                            uiState.error?.let { error -> item { RefreshError("Couldn't refresh: $error") } }
                            uiState.appUpdate?.let { update ->
                                item(key = "app-update") { AppUpdateCard(update, onUpdate = onUpdateApp) }
                            }
                            if (unit != null) {
                                uiState.headUnitError?.let { error -> item { RefreshError("Couldn't refresh ${unit.name}: $error") } }
                                uiState.agentUpdate?.let { update ->
                                    item(key = "agent-update") {
                                        AgentUpdateCard(update, unit.name, onOpen = { onOpenPackage(AGENT_PACKAGE_ID) })
                                    }
                                }
                            }
                            uiState.sections.forEach { section ->
                                sectionTitle(section.kind, uiState.filter)?.let { title ->
                                    item(key = "section-${section.kind.name}") {
                                        SectionHeader(title, section.rows.size.takeIf { section.kind == CatalogueSection.Kind.UPDATES })
                                    }
                                }
                                items(section.rows, key = { it.pkg.id }) { row ->
                                    PackageRow(row, onClick = { onOpenPackage(row.pkg.id) })
                                }
                            }
                            uiState.sourceName?.let { source ->
                                item {
                                    Text(
                                        "Source: $source",
                                        Modifier.fillMaxWidth().padding(16.dp),
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        style = MaterialTheme.typography.bodySmall,
                                        textAlign = TextAlign.Center,
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/** The text shown in place of the list, or null when there are packages to list. */
internal fun CatalogueUiState.emptyMessage(): String? = when {
    !loaded -> error?.let { "Couldn't load the catalogue: $it" }
    sections.isNotEmpty() -> null
    query.isNotBlank() || filter is CatalogueFilter.Category -> "No packages match."
    filter == CatalogueFilter.Installed -> "Nothing from this catalogue is installed on ${headUnit?.name}."
    filter == CatalogueFilter.Updates -> "No updates for ${headUnit?.name}."
    else -> "This catalogue has no packages."
}

/** The heading over a section's rows, or null for rows that have none. */
internal fun sectionTitle(kind: CatalogueSection.Kind, filter: CatalogueFilter): String? = when (kind) {
    CatalogueSection.Kind.PLAIN -> null
    CatalogueSection.Kind.UPDATES -> "Updates available"
    CatalogueSection.Kind.REST -> when (filter) {
        CatalogueFilter.Installed -> "Installed"
        is CatalogueFilter.Category -> categoryLabel(filter.id)
        else -> "All packages"
    }
}

/** Under a package's summary: what stands between the head unit and a version of it, if anything does. */
internal fun PackageStatus.note(): String? {
    val held = heldBack
    return when {
        !suits -> offered.agentRange?.let { "Needs agent ${rangeLabel(it)}" }
        held != null -> held.agentRange?.let { "${held.version} needs agent ${rangeLabel(it)}" }
        else -> null
    }
}

/** What the agent's update card says under its title: the agent that runs, and what waits for the newer one. */
internal fun agentUpdateText(update: AgentUpdate, headUnitName: String): String {
    val waiting = update.unblocks.map { it.label }
    val runs = "$headUnitName runs agent ${update.running}."
    return when (waiting.size) {
        0 -> runs
        1 -> "$runs ${waiting[0]} needs the newer one."
        else -> "$runs ${waiting[0]} and ${waiting.size - 1} more need the newer one."
    }
}

/**
 * What the app's update card says under its facts: where the update stands, or why it cannot begin. Null when
 * there is only the button to show.
 */
internal fun appUpdateStatus(update: AppUpdateView): String? {
    val offer = update.offer
    if (offer is AppUpdateOffer.Blocked) {
        return when (offer.reason) {
            AppUpdateOffer.Reason.OTHER_KEY ->
                "This copy can't update itself: it is a trial or test build, signed with another key than the " +
                    "release. To move to the release, uninstall this copy first; its saved head units and pairings go with it."
            AppUpdateOffer.Reason.ANDROID_TOO_OLD -> "It needs a newer version of Android than this phone has."
        }
    }
    return when (val state = update.state) {
        UpdateState.Idle -> WAITS_FOR_JOB.takeIf { update.waitsForJob }
        UpdateState.NeedsPermission ->
            "Android has to be told, once, that this app may install its own updates. Allow it in Android's " +
                "settings and come back: the update goes on from here."
        is UpdateState.Downloading -> "Downloading…"
        UpdateState.Installing -> "Installing. The app closes when the new version is in place, and may need opening again."
        UpdateState.Confirming -> "Android is asking whether to install the update."
        is UpdateState.Failed -> state.reason
    }
}

private const val WAITS_FOR_JOB = "A job is running on a head unit. Update the app once it has finished."

/** What the card's button does. */
internal enum class AppUpdateAction(val label: String) {
    UPDATE("Update"),
    RETRY("Try again"),

    /** Opens Android's settings, where the user allows this app to install apps. */
    ALLOW("Open settings"),
}

/** The card's button, or null when it has none: a copy that cannot take the release, or an update under way. */
internal fun appUpdateAction(update: AppUpdateView): AppUpdateAction? = when {
    update.offer !is AppUpdateOffer.Available -> null
    update.state == UpdateState.Idle -> AppUpdateAction.UPDATE
    update.state is UpdateState.Failed -> AppUpdateAction.RETRY
    update.state == UpdateState.NeedsPermission -> AppUpdateAction.ALLOW
    else -> null
}

/** Opens the page of Android's settings where this app is allowed to install apps, or the list of such apps. */
private fun openInstallSettings(context: Context) {
    val page = Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${context.packageName}"))
    try {
        context.startActivity(page)
    } catch (e: ActivityNotFoundException) {
        try {
            context.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES))
        } catch (e: ActivityNotFoundException) {
            // A phone with no such page: the card's words say what to look for.
        }
    }
}

/** Everything, then the head unit's two when its list is known, then the catalogue's categories. */
@Composable
private fun FilterRow(uiState: CatalogueUiState, onSelect: (CatalogueFilter) -> Unit) {
    LazyRow(
        contentPadding = PaddingValues(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { Chip("All", uiState.filter == CatalogueFilter.All) { onSelect(CatalogueFilter.All) } }
        if (uiState.headUnit != null) {
            item { Chip("Installed", uiState.filter == CatalogueFilter.Installed) { onSelect(CatalogueFilter.Installed) } }
            item {
                Chip("Updates", uiState.filter == CatalogueFilter.Updates, count = uiState.updateCount) { onSelect(CatalogueFilter.Updates) }
            }
        }
        items(uiState.categories, key = { it }) { category ->
            Chip(categoryLabel(category), uiState.filter == CatalogueFilter.Category(category)) {
                onSelect(CatalogueFilter.Category(category))
            }
        }
    }
}

@Composable
private fun Chip(label: String, selected: Boolean, count: Int = 0, onClick: () -> Unit) {
    FilterChip(
        selected = selected,
        onClick = onClick,
        label = {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(label)
                if (count > 0) CountBadge(count)
            }
        },
        leadingIcon = if (selected) {
            { Icon(painterResource(R.drawable.ic_check), contentDescription = null, Modifier.size(FilterChipDefaults.IconSize)) }
        } else {
            null
        },
    )
}

/** Amber is the app's colour for what is new: a count of updates, and the tag on a row that has one. */
@Composable
private fun CountBadge(count: Int) {
    Badge(containerColor = MaterialTheme.colorScheme.tertiary, contentColor = MaterialTheme.colorScheme.onTertiary) {
        Text("$count")
    }
}

/** How old a saved reading of the head unit is, to end a sentence with: "as of 2 days ago", or "just now" within the minute. */
internal fun asOf(seenAt: Long): String =
    if (System.currentTimeMillis() - seenAt < DateUtils.MINUTE_IN_MILLIS) "just now" else "as of ${DateUtils.getRelativeTimeSpanString(seenAt)}"

/** Shown with no link: whose list the rows are marked from, and how old it is. */
@Composable
private fun SavedListLine(unit: Holdings) {
    Row(
        Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Icon(
            painterResource(R.drawable.ic_history),
            contentDescription = null,
            Modifier.size(16.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Installed list from ${unit.name} · ${asOf(unit.seenAt)}",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

/** The connected head unit still has to restart for something that was installed; its own screen has the buttons. */
@Composable
private fun RestartBanner(text: String, onOpenHeadUnit: () -> Unit) {
    Surface(
        color = MaterialTheme.colorScheme.tertiaryContainer,
        contentColor = MaterialTheme.colorScheme.onTertiaryContainer,
        modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
    ) {
        Row(
            Modifier.padding(start = 16.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Icon(painterResource(R.drawable.ic_restart), contentDescription = null, Modifier.size(20.dp))
            Text(text, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            TextButton(
                onClick = onOpenHeadUnit,
                colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.onTertiaryContainer),
            ) { Text("Head unit") }
        }
    }
}

/**
 * A newer release of the app itself comes before everything: the app is in no app store, so this card is the
 * only place that says so. It carries the whole update: the button, the download, Android's question and a
 * failure's reason.
 */
@Composable
private fun AppUpdateCard(update: AppUpdateView, onUpdate: () -> Unit) {
    val release = update.offer.release
    val state = update.state
    val context = LocalContext.current
    val size = Formatter.formatShortFileSize(context, release.size)
    Card(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.primaryContainer,
            contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
        ),
    ) {
        // The whole width, whatever the words come to: the button stands at the card's end, not the text's.
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Icon(painterResource(R.drawable.ic_download), contentDescription = null)
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        "${stringResource(R.string.app_name)} ${release.versionName} is available",
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        "This phone has ${update.installedVersion}. The download is $size.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    release.notes.take(MAX_UPDATE_NOTES).forEach { note ->
                        Text("• $note", style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
            when (state) {
                is UpdateState.Downloading -> LinearProgressIndicator(
                    progress = { if (state.total > 0) (state.bytes.toFloat() / state.total).coerceIn(0f, 1f) else 0f },
                    modifier = Modifier.fillMaxWidth(),
                )
                UpdateState.Installing -> LinearProgressIndicator(Modifier.fillMaxWidth())
                else -> Unit
            }
            appUpdateStatus(update)?.let { status ->
                Text(
                    status,
                    color = if (state is UpdateState.Failed) MaterialTheme.colorScheme.error else Color.Unspecified,
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
            appUpdateAction(update)?.let { action ->
                Button(
                    onClick = { if (action == AppUpdateAction.ALLOW) openInstallSettings(context) else onUpdate() },
                    modifier = Modifier.align(Alignment.End),
                    // Allowing it does not end the app, so that need not wait for a job.
                    enabled = action == AppUpdateAction.ALLOW || !update.waitsForJob,
                ) { Text(action.label) }
            }
        }
    }
}

/** The card is one of several things on the screen: a long list of what is new is cut here. */
private const val MAX_UPDATE_NOTES = 6

/** A newer agent comes before the packages: some of them may wait for it, and it is updated in a job of its own. */
@Composable
private fun AgentUpdateCard(update: AgentUpdate, headUnitName: String, onOpen: () -> Unit) {
    Card(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.primaryContainer,
            contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
        ),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Icon(painterResource(R.drawable.ic_download), contentDescription = null)
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Agent ${update.version} is available", style = MaterialTheme.typography.titleMedium)
                    Text(agentUpdateText(update, headUnitName), style = MaterialTheme.typography.bodyMedium)
                }
            }
            Button(onClick = onOpen, Modifier.align(Alignment.End)) { Text("See the update") }
        }
    }
}

@Composable
private fun SectionHeader(title: String, count: Int?) {
    Row(
        Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 14.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            title,
            Modifier.semantics { heading() },
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.titleSmall,
        )
        if (count != null) CountBadge(count)
    }
}

@Composable
private fun RefreshError(text: String) {
    Text(
        text,
        Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
        color = MaterialTheme.colorScheme.error,
        style = MaterialTheme.typography.bodyMedium,
    )
}

@Composable
private fun PackageRow(row: CatalogueRow, onClick: () -> Unit) {
    val pkg = row.pkg
    val status = row.status
    ListItem(
        modifier = Modifier.clickable(onClick = onClick),
        leadingContent = { PackageIcon(pkg.name, 40.dp) },
        headlineContent = {
            // A package that no version of runs on this head unit's agent is listed, and stands back.
            Text(pkg.name, color = if (status?.suits == false) MaterialTheme.colorScheme.onSurfaceVariant else Color.Unspecified)
        },
        supportingContent = {
            Column {
                if (pkg.community) CommunityLabel(pkg.author)
                Text(pkg.summary, maxLines = 2, overflow = TextOverflow.Ellipsis)
                status?.note()?.let { Text(it, color = MaterialTheme.colorScheme.secondary, style = MaterialTheme.typography.bodySmall) }
            }
        },
        trailingContent = { if (status != null) RowStatus(status) },
    )
}

/** At the end of a row: an update with both versions, or that it is installed and in which, or the version on offer. */
@Composable
private fun RowStatus(status: PackageStatus) {
    val installed = status.installed
    Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        when {
            installed != null && status.updateAvailable -> {
                Text(
                    "Update",
                    Modifier
                        .background(MaterialTheme.colorScheme.tertiary, RoundedCornerShape(10.dp))
                        .padding(horizontal = 10.dp, vertical = 2.dp),
                    color = MaterialTheme.colorScheme.onTertiary,
                    style = MaterialTheme.typography.labelMedium,
                )
                val to = status.offered.version
                Text("$installed → $to", Modifier.semantics { contentDescription = "From $installed to $to" })
            }
            installed != null -> {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    Icon(
                        painterResource(R.drawable.ic_check),
                        contentDescription = null,
                        Modifier.size(14.dp),
                        tint = MaterialTheme.colorScheme.primary,
                    )
                    Text("Installed", color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.labelMedium)
                }
                Text(installed)
            }
            else -> Text(status.offered.version)
        }
    }
}

@Composable
private fun Message(text: String, modifier: Modifier = Modifier, onRetry: (() -> Unit)? = null) {
    Column(
        modifier.padding(24.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(text, style = MaterialTheme.typography.bodyLarge, textAlign = TextAlign.Center)
        if (onRetry != null) {
            TextButton(onClick = onRetry) { Text("Try again") }
        }
    }
}

/** Stand-in for the package's icon: its initial on a tinted tile. Icon files aren't fetched yet. */
@Composable
internal fun PackageIcon(name: String, size: Dp, modifier: Modifier = Modifier) {
    Box(
        modifier
            .size(size)
            .clip(RoundedCornerShape(size / 4))
            .background(MaterialTheme.colorScheme.secondaryContainer),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            name.trim().take(1).uppercase(),
            color = MaterialTheme.colorScheme.onSecondaryContainer,
            style = if (size >= 56.dp) MaterialTheme.typography.headlineSmall else MaterialTheme.typography.titleMedium,
        )
    }
}

/**
 * Marks a package from an outside developer, with its author where the catalogue names one: "Community · Jo
 * Bloggs". The source reviewed and signed it; it did not make it.
 */
@Composable
internal fun CommunityLabel(author: String, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(
            "Community",
            Modifier
                .background(MaterialTheme.colorScheme.secondaryContainer, RoundedCornerShape(10.dp))
                .padding(horizontal = 8.dp, vertical = 1.dp),
            color = MaterialTheme.colorScheme.onSecondaryContainer,
            style = MaterialTheme.typography.labelSmall,
        )
        if (author.isNotEmpty()) {
            Text(
                author,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.labelMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

/** Categories are published as lower-case ids; "hmi" is an initialism, the rest are plain words. */
internal fun categoryLabel(category: String): String =
    if (category == "hmi") "HMI" else category.replaceFirstChar { it.uppercase() }

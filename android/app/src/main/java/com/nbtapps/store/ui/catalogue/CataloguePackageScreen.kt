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
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.ui.headunits.HeadUnitIndicator

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CataloguePackageScreen(
    onBack: () -> Unit,
    onOpenPackage: (String) -> Unit,
    onInstall: (packageId: String, version: String) -> Unit,
    onOpenHeadUnit: (String) -> Unit,
    onAddHeadUnit: () -> Unit,
    onPairHeadUnit: (String) -> Unit,
    viewModel: CataloguePackageViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val pkg = uiState.pkg

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
                actions = { HeadUnitIndicator(onOpenHeadUnit, onAddHeadUnit, onPairHeadUnit) },
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
                Header(pkg)
                uiState.get?.let { get -> GetCard(get, onDownload = viewModel::download, onInstall = { onInstall(pkg.id, get.version.version) }) }
                pkg.latest?.let { latest ->
                    LatestVersionCard(latest)
                    if (latest.riskFlags.isNotEmpty()) RiskCard(latest)
                }
                if (uiState.dependencies.isNotEmpty()) DependenciesCard(uiState.dependencies, onOpenPackage)
                DetailsSections(uiState.details, onRetry = viewModel::retryDetails)
                if (pkg.versions.size > 1) VersionsCard(pkg.versions)
                Text(
                    listOfNotNull(pkg.id, uiState.sourceName).joinToString("\n"),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
private fun Header(pkg: CataloguePackage) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        PackageIcon(pkg.name, 56.dp)
        Column {
            Text(pkg.name, style = MaterialTheme.typography.headlineSmall)
            Text(
                listOf(pkg.author, categoryLabel(pkg.category)).filter { it.isNotEmpty() }.joinToString(" · "),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
    if (pkg.summary.isNotEmpty()) Text(pkg.summary, style = MaterialTheme.typography.bodyLarge)
}

/** The way from the catalogue to the head unit: download to the phone at home, install in the car. */
@Composable
private fun GetCard(get: GetState, onDownload: () -> Unit, onInstall: () -> Unit) {
    val context = LocalContext.current
    Section(if (get.installedVersion != null) "Installed: ${get.installedVersion}" else "Get it") {
        get.installedAsOf?.let { seenAt ->
            Text(
                "On ${get.headUnitName} ${asOf(seenAt)}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
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
        when {
            get.incompatible != null -> Text(get.incompatible, color = MaterialTheme.colorScheme.error)
            get.upToDate -> Text("${get.headUnitName} has this version or a newer one.")
            !get.connected -> Text(
                "Connect to ${get.headUnitName ?: "a head unit"} to ${if (get.installedVersion != null) "update" else "install"} it, " +
                    "from its name at the top of this screen. The download needs no car.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            else -> Button(onClick = onInstall, enabled = get.canInstall) {
                Text(if (get.installedVersion != null) "Update on ${get.headUnitName}" else "Install on ${get.headUnitName}")
            }
        }
    }
}

@Composable
private fun LatestVersionCard(version: PackageVersion) {
    Section("Version ${version.version}") {
        Text(versionFacts(version))
        version.requires.agent?.let { Text("Needs agent ${rangeLabel(it)}") }
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
    }
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
private fun DetailsSections(state: DetailsState, onRetry: () -> Unit) {
    when (state) {
        DetailsState.Loading -> Section("About") { CircularProgressIndicator() }
        is DetailsState.Failed -> Section("About") {
            Text("Couldn't load the description: ${state.reason}", color = MaterialTheme.colorScheme.error)
            TextButton(onClick = onRetry) { Text("Try again") }
        }
        is DetailsState.Loaded -> {
            val details = state.details
            if (details.description.isNotBlank()) Section("About") { Text(details.description) }
            if (details.changelog.any { it.notes.isNotEmpty() }) ChangelogCard(details)
        }
    }
}

@Composable
private fun ChangelogCard(details: PackageDetails) {
    Section("What's new") {
        details.changelog.filter { it.notes.isNotEmpty() }.forEach { entry ->
            Column {
                Text(entry.version, style = MaterialTheme.typography.titleSmall)
                entry.notes.forEach { note -> Text("• $note", style = MaterialTheme.typography.bodyMedium) }
            }
        }
    }
}

@Composable
private fun VersionsCard(versions: List<PackageVersion>) {
    Section("All versions") {
        versions.forEach { version ->
            Column {
                Text(version.version, style = MaterialTheme.typography.titleSmall)
                Text(versionFacts(version), style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

@Composable
private fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
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

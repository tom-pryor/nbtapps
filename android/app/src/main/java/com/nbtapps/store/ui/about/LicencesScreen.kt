// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.about

import android.content.res.AssetManager
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R
import java.io.IOException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** What a page read from the app's assets: [value] null until it has been read, and with [failed] where it could not be. */
private data class Loaded<T>(val value: T? = null, val failed: Boolean = false)

/** Reads [read] from the app's assets once, off the main thread. */
@Composable
private fun <T> loaded(read: (AssetManager) -> T): Loaded<T> {
    val assets = LocalContext.current.assets
    val state by produceState(Loaded<T>()) {
        value = withContext(Dispatchers.IO) {
            try {
                Loaded(read(assets))
            } catch (e: IOException) {
                Loaded(failed = true)
            } catch (e: IllegalArgumentException) {
                Loaded(failed = true)
            }
        }
    }
    return state
}

/** What a page says where its list or its text could not be read: the build puts both in, so this is not expected. */
internal const val LICENCES_MISSING = "The licences couldn't be read from this copy of the app."

/**
 * The projects whose code the app includes, each with its licence; a tap opens the licence's text
 * ([onOpenLicence], by its id). Only the phone app's own: the packages for the head unit have their own.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LicencesScreen(onBack: () -> Unit, onOpenLicence: (String) -> Unit) {
    val list = loaded(::readLicences)
    Scaffold(
        topBar = { TopAppBar(title = { Text("Open-source licences") }, navigationIcon = { BackButton(onBack) }) },
    ) { padding ->
        LazyColumn(Modifier.padding(padding).fillMaxSize()) {
            if (list.failed) item { Text(LICENCES_MISSING, Modifier.padding(16.dp), style = MaterialTheme.typography.bodyLarge) }
            items(list.value?.projects.orEmpty(), key = { it.name }) { project ->
                ListItem(
                    modifier = Modifier.clickable { onOpenLicence(project.licence.id) },
                    headlineContent = { Text(project.name) },
                    supportingContent = { Text(project.licence.title) },
                    trailingContent = {
                        Icon(
                            painterResource(R.drawable.ic_chevron_right),
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                )
            }
        }
    }
}

/**
 * The text of the licence [licenceId], with the projects under it named first. The text is the one that came with the
 * code, flowed so that it wraps at the screen's width ([shownLicenceText]): none of it is cut off.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LicenceScreen(licenceId: String, onBack: () -> Unit) {
    val page = loaded { assets ->
        val list = readLicences(assets)
        val licence = list.licences.firstOrNull { it.id == licenceId } ?: throw IllegalArgumentException("No licence $licenceId")
        Triple(licence, list.projects.filter { it.licence == licence }.map { it.name }, shownLicenceText(readLicenceText(assets, licence)))
    }
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(page.value?.first?.title.orEmpty(), maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = { BackButton(onBack) },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp)) {
            if (page.failed) Text(LICENCES_MISSING, style = MaterialTheme.typography.bodyLarge)
            page.value?.let { (_, projects, text) ->
                Text(
                    licenceUsedBy(projects),
                    Modifier.padding(bottom = 16.dp),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodyMedium,
                )
                SelectionContainer { Text(text, style = MaterialTheme.typography.bodySmall) }
            }
        }
    }
}

/** The line over a licence's text: which of the projects come under it. */
internal fun licenceUsedBy(projects: List<String>): String = when (projects.size) {
    0 -> ""
    1 -> "The licence of ${projects[0]}."
    else -> "The licence of ${projects.dropLast(1).joinToString(", ")} and ${projects.last()}."
}

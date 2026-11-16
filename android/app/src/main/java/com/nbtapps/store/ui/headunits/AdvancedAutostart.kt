// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R

/** The title of the row that holds the autostart box. */
internal const val ADVANCED = "Advanced"

/** The line under [ADVANCED]: how the box stands, so it is seen without opening the row. */
internal fun advancedAutostartLine(checked: Boolean): String = if (checked) "Autostart is on" else "Autostart is off"

/** The warning's title, with the box cleared. */
internal const val AUTOSTART_OFF_TITLE = "The agent won't start by itself"

/** The warning's body. */
internal const val AUTOSTART_OFF_TEXT =
    "With autostart off, you start the agent by hand on the car every time you want to use the app, after each start of the car."

/**
 * The autostart box of the pairing screens ([EnableAutostartBox], with its note), behind a row titled
 * [ADVANCED] whose second line says whether autostart is on ([advancedAutostartLine]). It is closed to begin with;
 * whether it is open survives a rotation. Open, with the box cleared, a warning says what that means. [checked],
 * [onCheckedChange] and [enabled] are the box's own.
 *
 * A screen shows it only where it would show the box: where the agent can be started with the car, or where that is
 * not known yet.
 */
@Composable
fun AdvancedAutostart(checked: Boolean, onCheckedChange: (Boolean) -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    var open by rememberSaveable { mutableStateOf(false) }
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        ExpandRow(ADVANCED, advancedAutostartLine(checked), open, onToggle = { open = !open })
        if (open) {
            EnableAutostartBox(checked, onCheckedChange, enabled = enabled)
            if (!checked) AutostartOffWarning()
        }
    }
}

/** What clearing the box means, in the theme's colours for a warning. */
@Composable
private fun AutostartOffWarning() {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.tertiaryContainer,
            contentColor = MaterialTheme.colorScheme.onTertiaryContainer,
        ),
    ) {
        Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
            Icon(painterResource(R.drawable.ic_warning), contentDescription = null)
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(AUTOSTART_OFF_TITLE, style = MaterialTheme.typography.titleSmall)
                Text(AUTOSTART_OFF_TEXT, style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

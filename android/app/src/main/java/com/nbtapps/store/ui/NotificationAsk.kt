// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.platform.LocalContext

/**
 * What [then] is done through: first Android's question for the notification permission, where it is not granted,
 * and then [then], whatever the answer. The notification of a job, a terminal or the SSH port for other apps is how it
 * stays in sight, and can be ended, once the app is left; none of them waits for it. Android asks no more once the
 * user has refused it twice, and [then] follows at once from there on.
 */
@Composable
fun rememberNotificationAsk(then: () -> Unit): () -> Unit {
    val context = LocalContext.current
    val current by rememberUpdatedState(then)
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { current() }
    return {
        if (context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
            current()
        } else {
            ask.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }
}

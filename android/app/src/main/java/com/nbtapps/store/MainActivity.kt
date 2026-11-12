// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import android.graphics.Color
import android.os.Bundle
import android.view.View
import android.view.ViewTreeObserver
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.runtime.getValue
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.nbtapps.store.data.update.AppUpdater
import com.nbtapps.store.data.update.SessionApkInstaller
import com.nbtapps.store.ui.navigation.NbtNavHost
import com.nbtapps.store.ui.navigation.StartViewModel
import com.nbtapps.store.ui.theme.NbtAppTheme
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject
import kotlinx.coroutines.launch

@AndroidEntryPoint
class MainActivity : ComponentActivity() {
    private val start: StartViewModel by viewModels()

    @Inject
    lateinit var installer: SessionApkInstaller

    @Inject
    lateinit var updater: AppUpdater

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The theme is dark on every phone, so the bars' icons are light even when the phone is in light mode.
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        setContent {
            NbtAppTheme {
                val startOnHeadUnits by start.startOnHeadUnits.collectAsStateWithLifecycle()
                startOnHeadUnits?.let { NbtNavHost(startOnHeadUnits = it) }
            }
        }
        holdFirstFrameUntilStartIsKnown()
        openUpdateConfirmations()
    }

    override fun onResume() {
        super.onResume()
        // Back from Android's settings, perhaps: an update that waited to be allowed goes on.
        updater.permissionMayHaveChanged()
    }

    /**
     * Android asks about an update of the app on a screen of its own, which only an app that is in front may
     * open. So it is opened from here, while this activity shows; a question that came while it did not waits
     * until it does.
     */
    private fun openUpdateConfirmations() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                installer.confirmation.collect { question ->
                    if (question != null) {
                        installer.confirmation.value = null
                        startActivity(question)
                    }
                }
            }
        }
    }

    /**
     * Which screen the app opens on waits for the saved head units to be read. Nothing is drawn until then, so
     * the system's launch screen stays up in place of an empty frame.
     */
    private fun holdFirstFrameUntilStartIsKnown() {
        val content = findViewById<View>(android.R.id.content)
        content.viewTreeObserver.addOnPreDrawListener(object : ViewTreeObserver.OnPreDrawListener {
            override fun onPreDraw(): Boolean {
                if (start.startOnHeadUnits.value == null) return false
                content.viewTreeObserver.removeOnPreDrawListener(this)
                return true
            }
        })
    }
}

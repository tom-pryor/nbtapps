// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import com.nbtapps.store.MainActivity
import com.nbtapps.store.ui.theme.NbtAppTheme
import dagger.hilt.android.AndroidEntryPoint

/**
 * Where another app hands this one a pairing code: see docs/pairing-code-handover.md. An intent that is not such a
 * request closes it at once. Once the phone is paired the app's own screens open, and this one closes; the link
 * it made stays up for them.
 */
@AndroidEntryPoint
class PairRequestActivity : ComponentActivity() {
    private val viewModel: PairRequestViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val request = intent.pairRequest()
        if (request == null) {
            finish()
            return
        }
        // Again after a rotation or a restart of the process, with the same code, which changes nothing.
        viewModel.handle(request)
        // As the app's own screens: the theme is dark on every phone, so the bars' icons are light.
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        setContent {
            NbtAppTheme {
                PairRequestScreen(
                    viewModel = viewModel,
                    onPaired = { name ->
                        Toast.makeText(this, "Paired with $name", Toast.LENGTH_SHORT).show()
                        openApp()
                    },
                    onOpenApp = ::openApp,
                    onClose = ::finish,
                )
            }
        }
    }

    /**
     * A request while this one is open: one with another code takes its place. One that is no request is
     * ignored here, not obeyed by closing: it would end what the user is in the middle of.
     */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        val request = intent.pairRequest() ?: return
        setIntent(intent)
        viewModel.handle(request)
    }

    /** The app's own screens, as they stand: an instance there is already is brought up, not a second made. */
    private fun openApp() {
        startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP))
        finish()
    }
}

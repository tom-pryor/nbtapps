// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import android.app.Activity
import android.app.Application
import android.os.Bundle
import com.nbtapps.store.data.ActivityForeground
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.model.AppVersion
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class NbtApplication : Application() {

    @Inject
    lateinit var foreground: ActivityForeground

    @Inject
    lateinit var installed: InstalledApp

    override fun onCreate() {
        super.onCreate()
        // The version a package's requires.app is held against, before anything reads it.
        AppVersion.current = AppVersion.of(installed.versionName)
        // Whether the app is in front of the user, from its activities' starts and stops: see [ActivityForeground].
        registerActivityLifecycleCallbacks(
            object : ActivityLifecycleCallbacks {
                override fun onActivityStarted(activity: Activity) = foreground.started()

                override fun onActivityStopped(activity: Activity) = foreground.stopped()

                override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = Unit

                override fun onActivityResumed(activity: Activity) = Unit

                override fun onActivityPaused(activity: Activity) = Unit

                override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit

                override fun onActivityDestroyed(activity: Activity) = Unit
            },
        )
    }
}

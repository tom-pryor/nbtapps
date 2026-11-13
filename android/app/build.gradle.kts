// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    // Kotlin support is built into AGP 9; only compiler plugins are applied here.
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
    alias(libs.plugins.hilt)
}

// The app's version, in this one place. Android takes an update only when its version code is higher than the
// installed one's, so the code is worked out from the name and rises whenever the name does:
// major * 10000 + minor * 100 + patch, which makes 0.1.0 the code 100 and leaves minor and patch two digits each.
val appVersion = "0.4.0"
val appVersionCode = appVersion.split('.').map { it.toIntOrNull() }.let { parts ->
    require(parts.size == 3 && parts.all { it != null && it >= 0 } && parts[1]!! < 100 && parts[2]!! < 100) {
        "Not a version of the form major.minor.patch, with minor and patch below 100: $appVersion"
    }
    parts[0]!! * 10000 + parts[1]!! * 100 + parts[2]!!
}

// `-Ptrial` makes the release build one that can go onto a phone on this machine's USB, for a trial on a car:
// signed with this machine's debug key, whether or not there is a release key, and not minified. Its code and
// its bindings are the release build's. The version's name ends in "-trial", so that it is not taken for a
// release; it is not one, and is not for handing on. A phone that has it cannot take a release as an update,
// because the two are signed with different keys: the trial is uninstalled first.
//   ./gradlew :app:installRelease -Ptrial
val trial = providers.gradleProperty("trial").isPresent

// The release key is not in this repo. A properties file beside it says where it is and how it is opened:
//   storeFile=android-release.jks      (a path from the properties file's own folder, or a whole path)
//   storePassword=...
//   keyAlias=...
//   keyPassword=...                    (left out or empty: the store's password, as in a store made by keytool)
// The file is ~/.config/nbtapps/keys/android-release.properties, or the one NBTAPPS_SIGNING names. Without it the
// release build is left unsigned (app-release-unsigned.apk), which no phone installs; tools/release.sh refuses
// to go on from there. Every release has to be signed with this one key: Android takes an update only from the
// key the installed app was signed with, so a lost key means no installed app can be updated again.
val releaseSigningFile = file(
    providers.environmentVariable("NBTAPPS_SIGNING").orNull
        ?: "${providers.systemProperty("user.home").get()}/.config/nbtapps/keys/android-release.properties",
)
val releaseSigning: Properties? = releaseSigningFile.takeIf { it.isFile }?.let { source ->
    Properties().apply { source.inputStream().use { load(it) } }
}
fun releaseSigningValue(name: String): String =
    releaseSigning?.getProperty(name)?.trim()?.takeIf { it.isNotEmpty() } ?: error("$releaseSigningFile has no $name")

android {
    namespace = "com.nbtapps.store"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.nbtapps.store"
        minSdk = 33
        targetSdk = 37
        versionCode = appVersionCode
        versionName = appVersion
    }

    signingConfigs {
        if (releaseSigning != null) {
            create("release") {
                storeFile = releaseSigningFile.resolveSibling(releaseSigningValue("storeFile"))
                storePassword = releaseSigningValue("storePassword")
                keyAlias = releaseSigningValue("keyAlias")
                // keytool's stores (PKCS12) open the key with the store's own password and never ask for another.
                keyPassword = releaseSigning.getProperty("keyPassword")?.trim()?.takeIf { it.isNotEmpty() }
                    ?: releaseSigningValue("storePassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = !trial
            if (trial) {
                signingConfig = signingConfigs.getByName("debug")
                versionNameSuffix = "-trial"
            } else if (releaseSigning != null) {
                signingConfig = signingConfigs.getByName("release")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.navigation.compose)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    debugImplementation(libs.androidx.compose.ui.tooling)

    implementation(libs.hilt.android)
    ksp(libs.hilt.compiler)
    implementation(libs.androidx.hilt.lifecycle.viewmodel.compose)

    implementation(libs.kotlinx.serialization.json)
    implementation(libs.androidx.datastore)
    // Ed25519 verification only, through the lightweight API: no provider is registered.
    implementation(libs.bouncycastle.provider)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
}

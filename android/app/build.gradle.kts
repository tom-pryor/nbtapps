// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.OutputStream
import javax.inject.Inject
import org.gradle.process.ExecOperations

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
val appVersion = "0.8.0"
val appVersionCode = appVersion.split('.').map { it.toIntOrNull() }.let { parts ->
    require(parts.size == 3 && parts.all { it != null && it >= 0 } && parts[1]!! < 100 && parts[2]!! < 100) {
        "Not a version of the form major.minor.patch, with minor and patch below 100: $appVersion"
    }
    parts[0]!! * 10000 + parts[1]!! * 100 + parts[2]!!
}

// `-Ptrial` makes the release build one that can go onto a phone on this machine's USB, for a trial on a car:
// signed with this machine's debug key, and not minified. Its code and its bindings are the release build's. The
// version's name ends in "-trial", so that it is not taken for a release; it is not one, and is not for handing
// on. A phone that has it cannot take a release as an update, because the two are signed with different keys: the
// trial is uninstalled first.
//   ./gradlew :app:installRelease -Ptrial
val trial = providers.gradleProperty("trial").isPresent

// The release build is never signed here: it is the same unsigned APK wherever it is built, and tools/sign.sh
// signs it with the release key, which no build sees. android/RELEASING.md says how a release goes out.

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

    buildTypes {
        release {
            isMinifyEnabled = !trial
            // No META-INF/version-control-info.textproto with the commit: AGP writes it from a plain git checkout
            // only, not from a worktree or an unpacked source, so the same sources would make two different APKs.
            // The release on GitHub names the commit instead.
            vcsInfo.include = false
            if (trial) {
                signingConfig = signingConfigs.getByName("debug")
                versionNameSuffix = "-trial"
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

    // On CI (GitHub Actions sets CI), lint leaves out the checks that look up the newest Gradle and library
    // versions on the internet: their findings change whenever something is published, not when this repo does.
    // Dependabot proposes those updates instead. On this machine they still show.
    lint {
        if (providers.environmentVariable("CI").isPresent) {
            disable += setOf("AndroidGradlePluginVersion", "NewerVersionAvailable", "GradleDependency")
        }
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

// The unit tests against the real agent (AgentBench and the tests that use it) depend on things Gradle does not see
// by itself: environment variables, files of the agent repo (headunit/) and of the catalogue repo, and the bench
// image in Docker. They are declared below as inputs of the unit tests, so that the tests run again when one of them
// changes and are up to date when none has. The image counts by its id, so that an image built again under the same
// name counts as a change too.

/** The id of a Docker image, or "none" when Docker or the image is not there: the tests that need it then skip. */
abstract class DockerImageId : ValueSource<String, DockerImageId.Parameters> {
    interface Parameters : ValueSourceParameters {
        val image: Property<String>
        val path: Property<String>
    }

    @get:Inject
    abstract val exec: ExecOperations

    override fun obtain(): String {
        // Docker is looked for on the PATH of this build, which the tests are given, and not on the PATH the Gradle
        // daemon started with, which it would otherwise be found on.
        val docker = parameters.path.get().split(File.pathSeparator).filter { it.isNotEmpty() }
            .map { File(it, "docker") }.firstOrNull { it.canExecute() } ?: return "none"
        val out = ByteArrayOutputStream()
        val asked = runCatching {
            exec.exec {
                commandLine(docker.path, "image", "inspect", "--format", "{{.Id}}", parameters.image.get())
                standardOutput = out
                errorOutput = OutputStream.nullOutputStream()
                isIgnoreExitValue = true
            }
        }.getOrNull()
        return if (asked?.exitValue == 0) out.toString().trim() else "none"
    }
}

tasks.withType<Test>().configureEach {
    // Read when the task runs, not when it is configured, so that a change of one keeps the configuration cache.
    // The tests resolve a relative path from the module folder, as these do.
    val moduleDir = projectDir
    fun variable(name: String) = providers.environmentVariable(name).filter { it.isNotEmpty() }
    fun folder(name: String, otherwise: String) = variable(name).orElse(otherwise).map { moduleDir.resolve(it).normalize() }

    // What the tests read, and what the agent repo's tools/bench.py reads for them.
    for (name in listOf("NBT_AGENT_REPO", "NBT_BENCH_IMAGE", "NBT_REQUIRE_BENCH", "NBT_CATALOGUE_REPO")) {
        inputs.property(name, variable(name).orElse(""))
    }
    inputs.property("benchImageId", providers.of(DockerImageId::class) {
        parameters.image.set(variable("NBT_BENCH_IMAGE").orElse("nbtapp-bench"))
        parameters.path.set(providers.environmentVariable("PATH").orElse(""))
    })

    // The agent repo's files the tests read themselves: the version AgentBench expects, bench.py, the vectors and keys
    // AgentVectorsInSyncTest compares, and the package tool RealAgentTest makes a large package with. Then what
    // `tools/bench.py testdata` makes the test packages from, which is the list the agent repo's Makefile makes its
    // own test data from, and the Dockerfile of the image it makes them in. The agent's sources are not here: the
    // tests run the agent of the bench image, whose id is the input above.
    val agentFiles = listOf(
        "agent/version.hpp", "tools/bench.py", "tools/bench/Dockerfile", "spec", "tests/keys", "tests/packages",
        "tests/make_packages.py", "tools/nbtpkg.py", "tools/plugin_pkg.py", "tools/agent_pkg.py",
        "plugin/NbtApp.manifest", "plugin/NbtApp.xml", "plugin/package/manifest.json.in",
    )
    inputs.files(folder("NBT_AGENT_REPO", "../../headunit").map { repo -> agentFiles.map { repo.resolve(it) } })
        .withPropertyName("agentRepoFiles")
        .withPathSensitivity(PathSensitivity.RELATIVE)
    // OfficialCatalogueTest compares the app's keys and address with these, when the catalogue repo is there.
    val catalogueFiles = listOf("keys/index.pub", "keys/package.pub", "tools/publish.py")
    inputs.files(folder("NBT_CATALOGUE_REPO", "../../../nbtapps-catalogue").map { repo -> catalogueFiles.map { repo.resolve(it) } })
        .withPropertyName("catalogueRepoFiles")
        .withPathSensitivity(PathSensitivity.RELATIVE)
}

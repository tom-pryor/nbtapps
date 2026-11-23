// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.OutputStream
import java.util.zip.ZipFile
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
val appVersion = "0.10.0"
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
    // Through its lightweight API only, no provider registered: Ed25519 verification, and the SSH key's signatures.
    implementation(libs.bouncycastle.provider)
    // The SSH client of the terminal (data/ssh/). It finds Bouncy Castle and uses its classes for Ed25519 and X25519.
    implementation(libs.jsch)

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

    // The test classes are shared out among several test JVMs at a time. They share nothing on this machine that
    // they write: each takes temporary folders of its own, and each agent that RealAgentTest and DebugTcpLinkTest
    // start is a container of its own with its socket in such a folder, DebugTcpLinkTest's also on a port that the
    // system hands out. Past four JVMs the suite is no quicker, since it then takes about as long as
    // JobRepositoryTest alone; half the processors leaves the rest of the machine, a CI runner's too, to the bench's
    // containers and everything else.
    maxParallelForks = (Runtime.getRuntime().availableProcessors() / 2).coerceIn(1, 4)
}

// The libraries' licence files ship in the APK as the libraries give them, in META-INF/. JSch's own (BSD) is its
// META-INF/LICENSE.txt, a name AGP leaves out of every APK, and another library has a file of that name too; so it
// goes in under a path of its own, META-INF/com.github.mwiede/jsch/LICENSE.txt, copied byte for byte from the jar the
// build uses. Its notices for JZlib (BSD) and jBCrypt (ISC), META-INF/LICENSE.JZlib.txt and LICENSE.jBCrypt.txt, go
// in as they are.
abstract class LibraryLicence : DefaultTask() {
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val jar: ConfigurableFileCollection

    @get:Input
    abstract val file: Property<String>

    @get:Input
    abstract val to: Property<String>

    @get:OutputDirectory
    abstract val output: DirectoryProperty

    @get:Inject
    abstract val archives: ArchiveOperations

    @get:Inject
    abstract val files: FileSystemOperations

    @TaskAction
    fun copy() {
        val from = file.get()
        val dest = to.get()
        files.sync {
            from(archives.zipTree(jar.singleFile)) {
                include(from)
                eachFile { path = dest }
            }
            includeEmptyDirs = false
            into(output)
        }
        check(output.file(dest).get().asFile.isFile) { "${jar.singleFile.name} has no $from" }
    }
}

val jschLicence = tasks.register<LibraryLicence>("jschLicence") {
    jar.from(configurations.detachedConfiguration(dependencies.create(libs.jsch.get())).apply { isTransitive = false })
    file = "META-INF/LICENSE.txt"
    to = "META-INF/com.github.mwiede/jsch/LICENSE.txt"
    output = layout.buildDirectory.dir("generated/licences/jsch")
}

// The app's screen "Open-source licences" lists the projects whose code it includes, from the list written by hand in
// src/main/assets/licences/licences.txt, and shows each licence's text, which the build puts beside the list in the
// assets: none of the texts is kept in this repo apart from the repo's own LICENSES/. The list's format is in its head.

/** The list of licences as the build reads it: the paragraphs that give a licence, and those that give a project. */
object LicenceList {
    class Paragraph(val kind: String, val name: String, val lines: List<Pair<String, String>>) {
        fun values(key: String) = lines.filter { it.first == key }.map { it.second }
    }

    fun paragraphs(text: String): List<Paragraph> {
        val paragraphs = mutableListOf<Paragraph>()
        var lines = mutableListOf<Pair<String, String>>()
        fun end() {
            if (lines.isNotEmpty()) paragraphs += Paragraph(lines[0].first, lines[0].second, lines.drop(1))
            lines = mutableListOf()
        }
        for (raw in text.lines()) {
            val line = raw.trim()
            when {
                line.startsWith("#") -> Unit
                line.isEmpty() -> end()
                else -> lines += line.substringBefore(' ') to line.substringAfter(' ', "").trim()
            }
        }
        end()
        return paragraphs
    }

    /** Every module of the graph under [root], as group:name, sorted: what the release build ships. */
    fun modules(root: ResolvedComponentResult): List<String> {
        val seen = mutableSetOf<ResolvedComponentResult>()
        val next = ArrayDeque(listOf(root))
        while (next.isNotEmpty()) {
            for (dependency in next.removeFirst().dependencies) {
                if (dependency is ResolvedDependencyResult && seen.add(dependency.selected)) next += dependency.selected
            }
        }
        return seen.mapNotNull { (it.id as? ModuleComponentIdentifier)?.let { id -> "${id.group}:${id.module}" } }.distinct().sorted()
    }
}

/**
 * Puts the text of each licence the list gives into licences/<id>.txt: the Apache License from the repo's LICENSES/,
 * Bouncy Castle's and JSch's from their own jars, byte for byte, JSch's three one after another (its own, then
 * JZlib's and jBCrypt's, which its jar carries). Fails where the list gives a licence that has no text here, or where
 * there is a text here for a licence the list does not give.
 */
abstract class LicenceTexts : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val list: RegularFileProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val apache: RegularFileProperty

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val bouncyCastleJar: ConfigurableFileCollection

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val jschJar: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val output: DirectoryProperty

    @get:Inject
    abstract val files: FileSystemOperations

    private fun entry(jar: File, name: String): ByteArray = ZipFile(jar).use { zip ->
        val found = zip.getEntry(name) ?: throw GradleException("${jar.name} has no $name")
        zip.getInputStream(found).use { it.readBytes() }
    }

    @TaskAction
    fun write() {
        val jsch = listOf("META-INF/LICENSE.txt", "META-INF/LICENSE.JZlib.txt", "META-INF/LICENSE.jBCrypt.txt")
        val texts = mapOf(
            "Apache-2.0" to apache.get().asFile.readBytes(),
            "Bouncy-Castle" to entry(bouncyCastleJar.singleFile, "META-INF/LICENSE.md"),
            // With a blank line between one and the next.
            "JSch" to jsch.joinToString("\n") { entry(jschJar.singleFile, it).decodeToString().trimEnd() + "\n" }.encodeToByteArray(),
        )
        val given = LicenceList.paragraphs(list.get().asFile.readText()).filter { it.kind == "licence" }.map { it.name }.toSet()
        if (given != texts.keys) {
            throw GradleException(
                "licences.txt gives the licences ${given.sorted()}, and the build has texts for ${texts.keys.sorted()}: " +
                    "a licence's text comes from the task licenceTexts in app/build.gradle.kts",
            )
        }
        files.delete { delete(output) }
        val dir = output.get().asFile.resolve("licences").apply { mkdirs() }
        texts.forEach { (id, bytes) -> dir.resolve("$id.txt").writeBytes(bytes) }
    }
}

/**
 * Checks the list against the release build's runtime classpath: every module on it is covered by exactly one
 * project of the list, each covers line of a project covers a module, each includes line names a file or folder of
 * the app's module, each project covers or includes something and names one licence the list gives, each licence
 * has a project, and no licence id or project name is given twice. A covers line is a group, which covers every
 * module of that group, or group:name, which covers that one module. An includes line is a path from the module's
 * folder, android/app/, to a file or to a folder with a file in it, outside build/.
 */
abstract class LicenceCheck : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val list: RegularFileProperty

    /** Every module of the release build's runtime classpath, as group:name. */
    @get:Input
    abstract val modules: ListProperty<String>

    /** The module's folder, which the includes lines are paths from; its files are [moduleFiles]. */
    @get:Internal
    abstract val moduleDir: DirectoryProperty

    /** Every file of the module's folder outside build/: those an includes line may name, by their paths in it. */
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val moduleFiles: ConfigurableFileCollection

    /** Which project covers each module, a line each: written once the check passes, so that it is up to date until the list, the modules or the module's files change. */
    @get:OutputFile
    abstract val report: RegularFileProperty

    @TaskAction
    fun check() {
        val paragraphs = LicenceList.paragraphs(list.get().asFile.readText())
        val licences = paragraphs.filter { it.kind == "licence" }.map { it.name }
        val projects = paragraphs.filter { it.kind == "project" }
        val problems = mutableListOf<String>()
        paragraphs.filter { it.kind != "licence" && it.kind != "project" }
            .forEach { problems += "A paragraph that is neither a licence nor a project: ${it.kind} ${it.name}" }
        // The app refuses a list with either twice, and its screen's list is keyed by the projects' names.
        licences.groupingBy { it }.eachCount().filterValues { it > 1 }.keys.forEach { problems += "The licence $it is given twice" }
        projects.groupingBy { it.name }.eachCount().filterValues { it > 1 }.keys.forEach { problems += "The project $it is given twice" }
        projects.forEach { project ->
            val licence = project.values("licence")
            if (licence.size != 1 || licence[0] !in licences) problems += "The project ${project.name} names no one licence that the list gives: $licence"
        }
        licences.filter { id -> projects.none { id in it.values("licence") } }.forEach { problems += "No project is under the licence $it" }
        projects.filter { it.values("covers").isEmpty() && it.values("includes").isEmpty() }
            .forEach { problems += "The project ${it.name} neither covers a module nor includes a file" }

        val dir = moduleDir.get().asFile
        val paths = moduleFiles.files.map { it.relativeTo(dir).invariantSeparatorsPath }
        fun there(line: String) = line.trimEnd('/').let { path -> paths.any { it == path || it.startsWith("$path/") } }
        projects.forEach { project ->
            project.values("includes").filterNot(::there)
                .forEach { problems += "The project ${project.name} includes $it, which is no file, nor folder with files, of android/app/ outside build/" }
        }

        val modules = modules.get()
        fun covers(line: String, module: String) = line == module || line == module.substringBefore(':')
        val coveredBy = modules.associateWith { module ->
            projects.filter { project -> project.values("covers").any { covers(it, module) } }.map { it.name }
        }
        val uncovered = coveredBy.filterValues { it.isEmpty() }.keys
        if (uncovered.isNotEmpty()) {
            problems += "Not covered by any project, so that the app does not show their licences: ${uncovered.joinToString(", ")}. " +
                "Add each to its project, or a project for it, under the licence its .pom names."
        }
        coveredBy.filterValues { it.size > 1 }.forEach { (module, names) -> problems += "$module is covered by more than one project: ${names.joinToString(", ")}" }
        projects.forEach { project ->
            project.values("covers").filter { line -> modules.none { covers(line, it) } }
                .forEach { problems += "The project ${project.name} covers $it, which is not on the release build's runtime classpath" }
        }
        if (problems.isNotEmpty()) throw GradleException("licences.txt does not fit the release build:\n  " + problems.joinToString("\n  "))
        report.get().asFile.writeText(coveredBy.entries.joinToString("") { (module, names) -> "$module ${names.single()}\n" })
    }
}

val licenceList = layout.projectDirectory.file("src/main/assets/licences/licences.txt")

val licenceTexts = tasks.register<LicenceTexts>("licenceTexts") {
    list = licenceList
    apache = layout.projectDirectory.file("../../LICENSES/Apache-2.0.txt")
    bouncyCastleJar.from(configurations.detachedConfiguration(dependencies.create(libs.bouncycastle.provider.get())).apply { isTransitive = false })
    jschJar.from(configurations.detachedConfiguration(dependencies.create(libs.jsch.get())).apply { isTransitive = false })
    output = layout.buildDirectory.dir("generated/licences/texts")
}

val checkLicences = tasks.register<LicenceCheck>("checkLicences") {
    group = "verification"
    description = "Checks that src/main/assets/licences/licences.txt covers each module of the release build exactly once, " +
        "and that the files it includes are there."
    list = licenceList
    moduleDir = layout.projectDirectory
    moduleFiles.from(layout.projectDirectory.asFileTree.matching { exclude("build") })
    report = layout.buildDirectory.file("reports/licences/covered.txt")
}

tasks.named("check") { dependsOn(checkLicences) }

androidComponents {
    onVariants { variant ->
        variant.sources.resources?.addGeneratedSourceDirectory(jschLicence, LibraryLicence::output)
        variant.sources.assets?.addGeneratedSourceDirectory(licenceTexts, LicenceTexts::output)
    }
    onVariants(selector().withBuildType("release")) { variant ->
        val graph = variant.runtimeConfiguration.incoming.resolutionResult.rootComponent
        checkLicences.configure { modules.set(graph.map(LicenceList::modules)) }
    }
}

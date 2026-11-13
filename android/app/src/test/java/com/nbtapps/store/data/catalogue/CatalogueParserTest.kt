// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.model.PackageDependency
import com.nbtapps.store.model.RiskFlag
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class CatalogueParserTest {

    private fun version(version: String, extra: String = "") =
        """{ "version": "$version", "url": "https://example.invalid/$version.nbtpkg", "size": 10, "sha256": "00",
            "manifestSha256": "11" $extra }"""

    private fun index(packages: String, schema: Int = 1) =
        """{ "schema": $schema, "source": { "id": "test", "name": "Test" }, "serial": 7, "packages": [$packages] }"""
            .encodeToByteArray()

    @Test
    fun `reads a package and its version`() {
        val parsed = CatalogueParser.parseIndex(
            index(
                """
                {
                  "id": "org.example.app", "name": "App", "summary": "Does things", "author": "Someone",
                  "category": "utilities", "details": "details/org.example.app.json",
                  "versions": [${
                    version(
                        "1.2.0",
                        """, "released": "2026-11-01", "requires": { "agent": ">=1.0.0" },
                           "depends": [{ "id": "org.example.lib", "version": ">=2.0.0 <3.0.0" }],
                           "flags": ["runs-scripts", "needs-reboot"]""",
                    )
                }]
                }
                """,
            ),
        )

        assertEquals(7, parsed.serial)
        assertEquals("Test", parsed.source.name)
        val pkg = parsed.packages.single()
        assertEquals("org.example.app", pkg.id)
        assertEquals("details/org.example.app.json", pkg.details)
        val latest = requireNotNull(pkg.latest)
        assertEquals("1.2.0", latest.version)
        assertEquals("00", latest.sha256)
        assertEquals("11", latest.manifestSha256)
        assertEquals(">=1.0.0", latest.requires.agent)
        assertEquals(listOf(PackageDependency("org.example.lib", ">=2.0.0 <3.0.0")), latest.depends)
        assertEquals(listOf(RiskFlag.RUNS_SCRIPTS, RiskFlag.NEEDS_REBOOT), latest.riskFlags)
    }

    @Test
    fun `optional fields can be left out`() {
        val pkg = CatalogueParser.parseIndex(
            index("""{ "id": "a", "name": "A", "versions": [${version("1.0.0")}] }"""),
        ).packages.single()

        assertEquals("", pkg.summary)
        assertNull(pkg.details)
        assertNull(pkg.latest?.requires?.agent)
        assertEquals(emptyList<PackageDependency>(), pkg.latest?.depends)
    }

    @Test
    fun `unknown fields and flags are ignored`() {
        val pkg = CatalogueParser.parseIndex(
            index(
                """{ "id": "a", "name": "A", "rating": 5,
                     "versions": [${version("1.0.0", """, "requires": { "agent": ">=1.0.0", "hmi": ["ID5"] }, "flags": ["future-flag", "runs-scripts"]""")}] }""",
            ),
        ).packages.single()

        assertEquals(listOf("future-flag", "runs-scripts"), pkg.latest?.flags)
        assertEquals(listOf(RiskFlag.RUNS_SCRIPTS), pkg.latest?.riskFlags)
    }

    @Test
    fun `versions come back newest first`() {
        val pkg = CatalogueParser.parseIndex(
            index(
                """{ "id": "a", "name": "A", "versions": [${version("1.9.0")}, ${version("1.10.0")}, ${version("1.2.0")}] }""",
            ),
        ).packages.single()

        assertEquals(listOf("1.10.0", "1.9.0", "1.2.0"), pkg.versions.map { it.version })
        assertEquals("1.10.0", pkg.latest?.version)
    }

    @Test
    fun `versions that are not semver are dropped, and packages left without one`() {
        val parsed = CatalogueParser.parseIndex(
            index(
                """
                { "id": "a", "name": "A", "versions": [${version("latest")}, ${version("1.0.0")}] },
                { "id": "b", "name": "B", "versions": [${version("nightly")}] },
                { "id": "c", "name": "C", "versions": [] }
                """,
            ),
        )

        assertEquals(listOf("a"), parsed.packages.map { it.id })
        assertEquals(listOf("1.0.0"), parsed.packages.single().versions.map { it.version })
    }

    @Test
    fun `an unsupported schema is refused`() {
        assertThrows(CatalogueFormatException::class.java) {
            CatalogueParser.parseIndex(index("", schema = 2))
        }
    }

    @Test
    fun `malformed json is reported as a format error`() {
        assertThrows(CatalogueFormatException::class.java) {
            CatalogueParser.parseIndex("{ not json".encodeToByteArray())
        }
        assertThrows(CatalogueFormatException::class.java) {
            CatalogueParser.parseIndex("""{ "schema": 1 }""".encodeToByteArray())
        }
    }

    @Test
    fun `a version that doesn't pin its manifest is a format error`() {
        assertThrows(CatalogueFormatException::class.java) {
            CatalogueParser.parseIndex(
                index(
                    """{ "id": "a", "name": "A", "versions": [
                         { "version": "1.0.0", "url": "https://example.invalid/a.nbtpkg", "size": 10, "sha256": "00" }] }""",
                ),
            )
        }
    }

    @Test
    fun `reads package details`() {
        val details = CatalogueParser.parseDetails(
            """
            { "id": "a", "description": "Long text", "homepage": "https://example.invalid/a",
              "changelog": [{ "version": "1.0.0", "notes": ["First release"] }] }
            """.encodeToByteArray(),
        )

        assertEquals("Long text", details.description)
        assertEquals(listOf("First release"), details.changelog.single().notes)
        assertEquals(emptyList<String>(), details.screenshots)
    }

    // ---- the app's own release ----

    private val appJson =
        """{ "versionCode": 200, "versionName": "0.2.0", "released": "2026-11-12",
             "url": "https://catalogue.example/v1/app/nbtapps-0.2.0.apk", "size": 3456747,
             "sha256": "${"ab".repeat(32)}", "certSha256": "${"cd".repeat(32)}", "minSdk": 33,
             "notes": ["Updates itself", "  ", " Fewer taps "], "later": true }"""

    private fun indexWithApp(app: String) =
        """{ "schema": 1, "source": { "id": "test", "name": "Test" }, "serial": 7,
             "packages": [{ "id": "a", "name": "A", "versions": [${version("1.0.0")}] }], "app": $app }""".encodeToByteArray()

    @Test
    fun `an index without the app's release has none`() {
        assertNull(CatalogueParser.parseIndex(index("")).app)
    }

    @Test
    fun `the app's release is read from the index`() {
        val app = requireNotNull(CatalogueParser.parseIndex(indexWithApp(appJson)).app)

        assertEquals(200L, app.versionCode)
        assertEquals("0.2.0", app.versionName)
        assertEquals("2026-11-12", app.released)
        assertEquals("https://catalogue.example/v1/app/nbtapps-0.2.0.apk", app.url)
        assertEquals(3456747L, app.size)
        assertEquals("ab".repeat(32), app.sha256)
        assertEquals("cd".repeat(32), app.certSha256)
        assertEquals(33, app.minSdk)
        assertEquals(listOf("Updates itself", "Fewer taps"), app.notes)
    }

    @Test
    fun `a release the app could not act on is left out and the packages are still listed`() {
        val bad = listOf(
            "null",
            "[]",
            "\"0.2.0\"",
            "{}",
            appJson.replace("\"versionCode\": 200", "\"versionCode\": [200]"),
            appJson.replace("\"versionCode\": 200", "\"versionCode\": 200.5"),
            appJson.replace("\"versionCode\": 200", "\"versionCode\": 0"),
            appJson.replace("\"versionName\": \"0.2.0\"", "\"versionName\": \" \""),
            appJson.replace("https://", "http://"),
            appJson.replace("\"size\": 3456747", "\"size\": 0"),
            appJson.replace("\"size\": 3456747", "\"size\": ${CatalogueParser.MAX_APP_BYTES + 1}"),
            appJson.replace("ab".repeat(32), "AB".repeat(32)),
            appJson.replace("cd".repeat(32), "cd".repeat(31)),
            appJson.replace("\"minSdk\": 33", "\"minSdk\": 0"),
            appJson.replace("\"minSdk\": 33,", ""),
            appJson.replace("[\"Updates itself\", \"  \", \" Fewer taps \"]", "\"Updates itself\""),
        )
        for (app in bad) {
            val parsed = CatalogueParser.parseIndex(indexWithApp(app))
            assertNull(app, parsed.app)
            assertEquals(app, listOf("a"), parsed.packages.map { it.id })
        }
    }

    private val sig = "ab".repeat(64)

    private fun indexWithCommunity(community: String, packages: String = """{ "id": "org.example.app", "name": "App", "versions": [${version("1.0.0")}] }""") =
        """{ "schema": 1, "source": { "id": "test", "name": "Test" }, "serial": 7, "packages": [$packages],
             "community": [$community] }""".encodeToByteArray()

    @Test
    fun `community packages join the source's own, marked and with their signatures`() {
        val parsed = CatalogueParser.parseIndex(
            indexWithCommunity(
                """{ "id": "io.github.someone.radio", "name": "Radio", "author": "Someone",
                     "versions": [${version("1.0.0", """, "signature": "$sig"""")}, ${version("1.1.0", """, "signature": "$sig"""")}] }""",
            ),
        )

        assertEquals(listOf("org.example.app", "io.github.someone.radio"), parsed.packages.map { it.id })
        val (official, community) = parsed.packages
        assertFalse(official.community)
        assertNull(official.latest?.signature)
        assertTrue(community.community)
        assertEquals("Someone", community.author)
        assertEquals(listOf("1.1.0", "1.0.0"), community.versions.map { it.version })
        assertEquals(sig, community.latest?.signature)
        assertTrue(community.latest!!.detachedSignature)
    }

    @Test
    fun `an index without community packages lists only the source's own`() {
        val parsed = CatalogueParser.parseIndex(index("""{ "id": "a", "name": "A", "versions": [${version("1.0.0")}] }"""))
        assertEquals(listOf("a"), parsed.packages.map { it.id })
        assertFalse(parsed.packages.single().community)
    }

    @Test
    fun `the source's own versions never carry a detached signature, nor are they marked community by the file`() {
        val pkg = CatalogueParser.parseIndex(
            index("""{ "id": "a", "name": "A", "community": true, "versions": [${version("1.0.0", """, "signature": "$sig"""")}] }"""),
        ).packages.single()

        assertFalse(pkg.community)
        assertNull(pkg.latest?.signature)
    }

    @Test
    fun `community packages that cannot be trusted are left out, and the rest are listed`() {
        fun signed(v: String) = version(v, """, "signature": "$sig"""")
        val parsed = CatalogueParser.parseIndex(
            indexWithCommunity(
                listOf(
                    // An id reserved for the source's own, which the agent lets the source's key sign.
                    """{ "id": "org.nbtapp.radio", "name": "R", "versions": [${signed("1.0.0")}] }""",
                    // The source's own package, under the same id.
                    """{ "id": "org.example.app", "name": "Impostor", "versions": [${signed("2.0.0")}] }""",
                    // Not a package id.
                    """{ "id": "Not An Id", "name": "N", "versions": [${signed("1.0.0")}] }""",
                    // Malformed: no name.
                    """{ "id": "io.github.a.nameless", "versions": [${signed("1.0.0")}] }""",
                    // Versions without a usable signature, and none left.
                    """{ "id": "io.github.a.unsigned", "name": "U", "versions": [${version("1.0.0")}, ${version("1.1.0", """, "signature": "${sig.uppercase()}"""")}, ${version("1.2.0", """, "signature": "${sig.drop(2)}"""")}] }""",
                    // Kept, without its unsigned version.
                    """{ "id": "io.github.a.kept", "name": "K", "versions": [${signed("1.0.0")}, ${version("1.1.0")}] }""",
                    // The same id again: the first is kept.
                    """{ "id": "io.github.a.kept", "name": "K2", "versions": [${signed("9.0.0")}] }""",
                ).joinToString(","),
            ),
        )

        assertEquals(listOf("org.example.app", "io.github.a.kept"), parsed.packages.map { it.id })
        assertEquals("App", parsed.packages[0].name)
        assertEquals(listOf("1.0.0"), parsed.packages[1].versions.map { it.version })
        assertEquals("K", parsed.packages[1].name)
    }

    @Test
    fun `a community key that is not a list is ignored`() {
        val parsed = CatalogueParser.parseIndex(
            """{ "schema": 1, "source": { "id": "t", "name": "T" }, "serial": 1, "packages": [], "community": { "x": 1 } }""".encodeToByteArray(),
        )
        assertTrue(parsed.packages.isEmpty())
    }

    @Test
    fun `an index that is not an object is malformed`() {
        assertThrows(CatalogueFormatException::class.java) { CatalogueParser.parseIndex("[]".encodeToByteArray()) }
        assertThrows(CatalogueFormatException::class.java) { CatalogueParser.parseIndex("{".encodeToByteArray()) }
    }
}

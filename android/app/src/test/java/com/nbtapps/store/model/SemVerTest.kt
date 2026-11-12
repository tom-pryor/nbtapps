// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SemVerTest {

    private fun v(text: String) = requireNotNull(SemVer.parseOrNull(text)) { "$text should parse" }

    @Test
    fun `parses the parts of a version`() {
        assertEquals(SemVer(1, 2, 3), v("1.2.3"))
        assertEquals(SemVer(1, 0, 0, listOf("beta", "2")), v("1.0.0-beta.2"))
        assertEquals(SemVer(0, 0, 0), v("0.0.0"))
        assertEquals(SemVer(999999999, 0, 1), v("999999999.0.1"))
    }

    @Test
    fun `build metadata does not affect the version`() {
        assertEquals(v("1.2.3"), v("1.2.3+build.7"))
        assertEquals(v("1.2.3-rc.1"), v("1.2.3-rc.1+5"))
    }

    @Test
    fun `rejects text that is not a full version`() {
        listOf(
            "", "1", "1.2", "1.2.3.4", "v1.2.3", "1.2.x", "1.2.3-", "1.2.3-a..b", "1.2.3+", " 1.2.3", "1.2.3 ",
            "01.2.3", "1.02.3", "1.2.03", "1000000000.0.0", "1.2.3-béta", "1.2.3+a+b", "1.2.3-" + "a".repeat(60),
        ).forEach {
            assertNull("$it should not parse", SemVer.parseOrNull(it))
        }
    }

    @Test
    fun `numbers compare as numbers, not text`() {
        assertTrue(v("1.10.0") > v("1.9.0"))
        assertTrue(v("2.0.0") > v("1.99.99"))
        assertTrue(v("1.0.10") > v("1.0.9"))
    }

    @Test
    fun `a release ranks above its pre-releases`() {
        assertTrue(v("1.0.0") > v("1.0.0-rc.1"))
        assertTrue(v("1.0.0-rc.1") < v("1.0.0"))
        assertTrue(v("1.0.1-alpha") > v("1.0.0"))
    }

    @Test
    fun `pre-releases follow semver precedence`() {
        val ordered = listOf(
            "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
            "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0",
        ).map(::v)
        assertEquals(ordered, ordered.shuffled().sorted())
    }

    @Test
    fun `numeric pre-release identifiers compare as numbers whatever their length`() {
        assertTrue(v("1.0.0-99999999999999999999") < v("1.0.0-100000000000000000000"))
        assertEquals(0, v("1.0.0-007").compareTo(v("1.0.0-7")))
        assertTrue(v("1.0.0-9") < v("1.0.0-a"))
    }

    @Test
    fun `a range holds when every comparator does`() {
        val range = requireNotNull(VersionRange.parseOrNull(">=2.0.0 <3.0.0"))
        assertTrue(range.matches(v("2.0.0")))
        assertTrue(range.matches(v("2.9.9")))
        assertFalse(range.matches(v("3.0.0")))
        assertFalse(range.matches(v("1.9.9")))
        assertFalse(range.matches(v("2.0.0-rc.1")))

        assertTrue(VersionRange.matches("1.2.0", "1.2.0"))
        assertTrue(VersionRange.matches("1.2.0", "=1.2.0"))
        assertFalse(VersionRange.matches("1.2.1", "1.2.0"))
        assertTrue(VersionRange.matches("1.2.1", ">1.2.0 <=1.2.1"))
        assertTrue(VersionRange.matches("1.2.1", null))
        assertTrue(VersionRange.matches("1.2.1", ""))
        assertFalse(VersionRange.matches("latest", null))
        assertFalse(VersionRange.matches("1.2.1", "about 1"))
    }

    @Test
    fun `a range is comparators separated by one space and nothing else`() {
        listOf(">=1.0.0", "<2.0.0", "1.0.0 2.0.0", ">=1.0.0-rc.1 <2.0.0+build").forEach {
            assertNotNull("$it should parse", VersionRange.parseOrNull(it))
        }
        listOf("", " >=1.0.0", ">=1.0.0 ", ">=1.0.0  <2.0.0", ">= 1.0.0", "~1.0.0", "^1.0.0", "1.x", ">=1.0", "1.0.0,2.0.0", "=>1.0.0")
            .forEach { assertNull("$it should not parse", VersionRange.parseOrNull(it)) }
    }
}

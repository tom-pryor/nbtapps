// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.appsettings

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * A licence's text as its page shows it ([shownLicenceText]): on samples, and on the real texts the app ships, which
 * must keep every word in its order.
 */
class LicenceTextTest {

    /**
     * A file of META-INF in the jar on the tests' classpath whose name begins with [jar]: other jars have files of these
     * names. The tests' classpath holds the jar twice, as it is and as AGP's transform copied it; both must give the same.
     */
    private fun fromJar(jar: String, name: String): String {
        val texts = javaClass.classLoader!!.getResources(name).toList().filter { "/$jar" in it.toString() }.map { it.readText() }
        assertTrue("no jar $jar* with $name on the classpath", texts.isNotEmpty())
        return texts.distinct().single()
    }

    /** JSch's three texts, as the build puts them together: its own, then JZlib's and jBCrypt's, with a blank line between. */
    private val jsch by lazy {
        listOf("META-INF/LICENSE.txt", "META-INF/LICENSE.JZlib.txt", "META-INF/LICENSE.jBCrypt.txt")
            .joinToString("\n") { fromJar("jsch-", it).trimEnd() + "\n" }
    }

    private val bouncyCastle by lazy { fromJar("bcprov-", "META-INF/LICENSE.md") }

    /** The repo's copy, which the build puts in the app as it is. The tests run in android/app. */
    private val apache by lazy { File("../../LICENSES/Apache-2.0.txt").readText() }

    private fun words(text: String) = text.split(Regex("""\s+""")).filter { it.isNotEmpty() }

    @Test
    fun `lines that were full join, and a line that ended short of the width stays`() {
        val text = """
            Copyright (c) 2002-2015 Somebody, Some Firm
            All rights reserved.

            Redistribution and use in source and binary forms, with or without
            modification, are permitted provided that the following conditions are
            met. A line that is the last of its paragraph ends it.

            A short line, ended on purpose.
            And another under it.
        """.trimIndent()
        assertEquals(
            """
            Copyright (c) 2002-2015 Somebody, Some Firm
            All rights reserved.

            Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met. A line that is the last of its paragraph ends it.

            A short line, ended on purpose.
            And another under it.
            """.trimIndent(),
            flowLicenceText(text),
        )
    }

    @Test
    fun `a rule, a copyright line and list items keep their lines, and an item's own lines join it without their indent`() {
        val text = listOf(
            "A heading line that is long enough to set the width of all this text here",
            "--------------------------------------------------------------------------",
            "Copyright (c) 2000-2026 Some Firm Inc. of a long name (https://example.org).",
            "Permission is granted to anyone, free of charge, to do with this as they wish",
            "",
            "  1. Redistributions of source code must retain the above copyright notice,",
            "     this list of conditions and the following disclaimer.",
            "  2. Redistributions in binary form must reproduce the above copyright ",
            "     notice.",
        ).joinToString("\n")
        assertEquals(
            listOf(
                "A heading line that is long enough to set the width of all this text here",
                "--------------------------------------------------------------------------",
                "Copyright (c) 2000-2026 Some Firm Inc. of a long name (https://example.org).",
                "Permission is granted to anyone, free of charge, to do with this as they wish",
                "",
                "  1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.",
                "  2. Redistributions in binary form must reproduce the above copyright notice.",
            ).joinToString("\n"),
            flowLicenceText(text),
        )
    }

    @Test
    fun `a comment frame is taken off, its blank lines kept, and an unclosed one is left as it is`() {
        val framed = listOf(
            "Its licence:",
            "",
            "/*",
            " * Copyright (c) 2006 Somebody <somebody@example.org>",
            " *",
            " * Permission to use, copy, modify, and distribute this software for any",
            " * purpose is hereby granted.",
            " */",
        ).joinToString("\n")
        assertEquals(
            listOf(
                "Its licence:",
                "",
                "Copyright (c) 2006 Somebody <somebody@example.org>",
                "",
                "Permission to use, copy, modify, and distribute this software for any purpose is hereby granted.",
            ).joinToString("\n"),
            flowLicenceText(framed),
        )
        val unclosed = "/*\n * Not a frame, as it is never closed"
        assertEquals(unclosed, flowLicenceText(unclosed))
    }

    @Test
    fun `a paragraph in bold markers loses them, and nothing else does`() {
        assertEquals("A\n\nTHE SOFTWARE\nIS PROVIDED.\n\nC", withoutBoldMarkers("A\n\n**THE SOFTWARE\nIS PROVIDED.**\n\nC"))
        assertEquals("THE SOFTWARE.", withoutBoldMarkers("**THE SOFTWARE.**"))
        for (unchanged in listOf("**Only at the start", "Only at the end**", "In **the** middle", "**", "Two\n\n**Paragraphs\n\nhalf bold**")) {
            assertEquals(unchanged, withoutBoldMarkers(unchanged))
        }
    }

    @Test
    fun `the Apache License, each paragraph one line, comes out as it is`() {
        assertEquals(apache, shownLicenceText(apache))
    }

    @Test
    fun `JSch's texts keep every word in order, less only the comment frame of jBCrypt's`() {
        val frame = listOf("/*", "*", "*/")
        val original = words(jsch)
        assertEquals(mapOf("/*" to 1, "*" to 13, "*/" to 1), original.filter { it in frame }.groupingBy { it }.eachCount())
        assertEquals(original.filterNot { it in frame }, words(shownLicenceText(jsch)))
    }

    @Test
    fun `JSch's texts flow, with the copyright lines, the rules and the list items kept`() {
        val lines = shownLicenceText(jsch).lines()
        val copyright = lines.indexOf("Copyright (c) 2002-2015 Atsuhiko Yamanaka, JCraft,Inc.")
        assertEquals("All rights reserved.", lines[copyright + 1])
        assertEquals("-".repeat(78), lines[copyright - 1])
        assertTrue(lines.contains("JSch 0.0.* was released under the GNU LGPL license.  Later, we have switched over to a BSD-style license."))
        assertTrue(lines.contains("Copyright (c) 2000-2011 ymnk, JCraft,Inc. All rights reserved."))
        assertTrue(lines.contains("  1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer."))
        assertTrue(lines.contains("  3. The names of the authors may not be used to endorse or promote products derived from this software without specific prior written permission."))
        val warranties = lines.filter { it.startsWith("THIS SOFTWARE IS PROVIDED ``AS IS''") }
        assertEquals(2, warranties.size)
        assertTrue(warranties.all { it.endsWith("EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.") })
        val jBCrypt = lines.indexOf("Copyright (c) 2006 Damien Miller <djm@mindrot.org>")
        assertEquals(listOf("jBCrypt is subject to the following license:", ""), lines.subList(jBCrypt - 2, jBCrypt))
        assertEquals("", lines[jBCrypt + 1])
        assertTrue(lines[jBCrypt + 2].startsWith("Permission to use, copy,") && lines[jBCrypt + 2].endsWith("appear in all copies."))
        assertEquals("", lines[jBCrypt + 3])
        assertTrue(lines[jBCrypt + 4].startsWith("THE SOFTWARE IS PROVIDED \"AS IS\"") && lines[jBCrypt + 4].endsWith("PERFORMANCE OF THIS SOFTWARE."))
        assertEquals(listOf(""), lines.drop(jBCrypt + 5))
    }

    @Test
    fun `Bouncy Castle's text keeps every word in order, less only the bold markers of its warranty paragraph`() {
        val original = words(bouncyCastle)
        assertEquals(1, original.count { it == "**THE" })
        assertEquals(1, original.count { it == "SOFTWARE.**" })
        val expected = original.map {
            when (it) {
                "**THE" -> "THE"
                "SOFTWARE.**" -> "SOFTWARE."
                else -> it
            }
        }
        assertEquals(expected, words(shownLicenceText(bouncyCastle)))
    }

    @Test
    fun `Bouncy Castle's text flows, with its title and copyright lines kept`() {
        val lines = shownLicenceText(bouncyCastle).lines()
        assertEquals("MIT License (https://opensource.org/licenses/MIT)", lines[0])
        assertEquals("", lines[1])
        assertTrue(lines[2].startsWith("Copyright (c) ") && lines[2].endsWith("(https://www.bouncycastle.org)."))
        assertTrue(lines[3].startsWith("Permission is hereby granted") && lines[3].endsWith("substantial portions of the Software."))
        assertEquals("", lines[4])
        assertTrue(lines[5].startsWith("THE SOFTWARE IS PROVIDED") && lines[5].endsWith("OTHER DEALINGS IN THE SOFTWARE."))
        assertEquals(listOf(""), lines.drop(6))
    }
}

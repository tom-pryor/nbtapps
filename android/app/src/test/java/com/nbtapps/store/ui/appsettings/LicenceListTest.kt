// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.appsettings

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

/** The list of licences as the app reads it ([parseLicences]): the one in the app's assets, and lists that are not one. */
class LicenceListTest {

    /** The list the app ships, read from the module folder, where the tests run. */
    private val shipped by lazy { parseLicences(File("src/main/assets/$LICENCE_LIST").readText()) }

    @Test
    fun `the shipped list gives each project with its licence, in its order`() {
        val apache = Licence("Apache-2.0", "Apache License 2.0")
        assertEquals(
            listOf(
                apache,
                Licence("Bouncy-Castle", "MIT License"),
                Licence("JSch", "BSD and ISC licences"),
            ),
            shipped.licences,
        )
        assertEquals(
            listOf(
                "AndroidX" to "Apache-2.0",
                "Kotlin and kotlinx" to "Apache-2.0",
                "JetBrains Java Annotations" to "Apache-2.0",
                "Dagger and Hilt" to "Apache-2.0",
                "Guava: ListenableFuture" to "Apache-2.0",
                "Okio" to "Apache-2.0",
                "JSpecify" to "Apache-2.0",
                "JSR-305 annotations" to "Apache-2.0",
                "javax.inject and Jakarta Dependency Injection" to "Apache-2.0",
                "Material Icons" to "Apache-2.0",
                "Bouncy Castle" to "Bouncy-Castle",
                "JSch, with JZlib and jBCrypt" to "JSch",
            ),
            shipped.projects.map { it.name to it.licence.id },
        )
        assertEquals("licences/Apache-2.0.txt", apache.textPath)
    }

    @Test
    fun `comments and blank lines are passed over, and a project's covers lines are the build's`() {
        val list = parseLicences(
            """
            # A comment
            licence MIT
            title MIT License

               # Another, indented
            project Example
            licence MIT
            covers org.example
            covers org.other:one
            """.trimIndent(),
        )
        assertEquals(LicenceList(listOf(Licence("MIT", "MIT License")), listOf(LicensedProject("Example", Licence("MIT", "MIT License")))), list)
    }

    @Test
    fun `a project may include files of the app's source in place of modules, or as well`() {
        val mit = Licence("MIT", "MIT License")
        val list = parseLicences(
            """
            licence MIT
            title MIT License

            project Icons
            licence MIT
            includes src/main/res/drawable/ic_one.xml
            includes src/main/res/drawable/more

            project Both
            licence MIT
            covers org.example
            includes src/main/assets/example.txt
            """.trimIndent(),
        )
        assertEquals(LicenceList(listOf(mit), listOf(LicensedProject("Icons", mit), LicensedProject("Both", mit))), list)
    }

    @Test
    fun `a list that is not of the form is refused`() {
        val licence = "licence MIT\ntitle MIT License\n\n"
        for (bad in listOf(
            "${licence}project Example\nlicence GPL\ncovers org.example",
            "${licence}project Example\ncovers org.example",
            "${licence}project Example\nlicence MIT",
            "${licence}project Example\nlicence MIT\nincludes",
            "${licence}project Example\nincludes src/main/res/drawable/ic_one.xml",
            "${licence}project Example\nlicence MIT\nlicence MIT\ncovers org.example",
            "${licence}project Example\nlicence MIT\ncovers org.example\nhomepage https://example.org",
            "${licence}module org.example",
            "licence MIT\n\nproject Example\nlicence MIT\ncovers org.example",
            "${licence}${licence}project Example\nlicence MIT\ncovers org.example",
            "${licence}project Example\nlicence\ncovers org.example",
        )) {
            assertThrows(bad, IllegalArgumentException::class.java) { parseLicences(bad) }
        }
    }

    @Test
    fun `a list that gives a licence id or a project name twice is refused`() {
        val project = "project Example\nlicence MIT\ncovers org.example\n\n"
        for (bad in listOf(
            "licence MIT\ntitle MIT License\n\nlicence MIT\ntitle Another title\n\n$project",
            "licence MIT\ntitle MIT License\n\n${project}project Example\nlicence MIT\nincludes src/main/assets/example.txt",
        )) {
            assertThrows(bad, IllegalArgumentException::class.java) { parseLicences(bad) }
        }
    }
}

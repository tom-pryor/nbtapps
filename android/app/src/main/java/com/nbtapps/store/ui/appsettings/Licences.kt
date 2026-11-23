// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.appsettings

import android.content.res.AssetManager
import java.io.IOException

/** A licence of code the app includes. Its text is in the app's assets, at [textPath]. */
data class Licence(val id: String, val title: String) {
    val textPath: String get() = "$LICENCE_FOLDER/$id.txt"
}

/** A project whose code the app includes, under [licence]. */
data class LicensedProject(val name: String, val licence: Licence)

/** The projects whose code the app includes, in the order the list gives them. */
data class LicenceList(val licences: List<Licence>, val projects: List<LicensedProject>)

/** Where the list and the licences' texts are in the app's assets. */
const val LICENCE_FOLDER = "licences"

/** The list itself, written by hand: `android/app/src/main/assets/licences/licences.txt`, which says its format. */
const val LICENCE_LIST = "$LICENCE_FOLDER/licences.txt"

/** The keys of a project's lines: its licence, and the modules it covers or the files it includes. */
private val PROJECT_KEYS = setOf("licence", "covers", "includes")

/**
 * Reads the list of licences and projects: paragraphs of `key value` lines, each begun by `licence ID` or
 * `project NAME`, with `#` lines as comments. A licence has a `title`; a project names its `licence` by id, and the
 * Maven groups or modules it `covers`, or the files of the app's source it `includes`, or both, which only the build's
 * check reads. Throws [IllegalArgumentException] for a list that is not of this form, that gives a licence id or a
 * project name twice, or whose project names a licence it does not give.
 */
fun parseLicences(text: String): LicenceList {
    val paragraphs = mutableListOf<List<Pair<String, String>>>()
    var current = mutableListOf<Pair<String, String>>()
    for (raw in text.lines()) {
        val line = raw.trim()
        if (line.startsWith("#")) continue
        if (line.isEmpty()) {
            if (current.isNotEmpty()) paragraphs += current
            current = mutableListOf()
            continue
        }
        val key = line.substringBefore(' ')
        val value = line.substringAfter(' ', "").trim()
        require(value.isNotEmpty()) { "A line with no value: $line" }
        current += key to value
    }
    if (current.isNotEmpty()) paragraphs += current

    val licences = mutableListOf<Licence>()
    val named = mutableListOf<Pair<String, String>>()
    for (paragraph in paragraphs) {
        val (kind, name) = paragraph.first()
        fun one(key: String) = paragraph.filter { it.first == key }.singleOrNull()?.second
            ?: throw IllegalArgumentException("$kind $name has no one $key")
        val keys = paragraph.drop(1).map { it.first }.toSet()
        when (kind) {
            "licence" -> {
                require(keys == setOf("title")) { "licence $name has other lines than its title" }
                licences += Licence(name, one("title"))
            }
            "project" -> {
                require("licence" in keys && keys.size > 1 && keys.all { it in PROJECT_KEYS }) {
                    "project $name needs a licence and what it covers or includes, and nothing else"
                }
                named += name to one("licence")
            }
            else -> throw IllegalArgumentException("A paragraph begins with neither licence nor project: $kind $name")
        }
    }
    val byId = licences.associateBy { it.id }
    require(byId.size == licences.size) { "A licence is given twice" }
    // The screen's list is keyed by the projects' names.
    val twice = named.groupingBy { it.first }.eachCount().filterValues { it > 1 }.keys
    require(twice.isEmpty()) { "A project is given twice: ${twice.joinToString(", ")}" }
    val projects = named.map { (name, id) ->
        LicensedProject(name, byId[id] ?: throw IllegalArgumentException("project $name names licence $id, which the list does not give"))
    }
    return LicenceList(licences, projects)
}

/** The list as the app has it. Throws [IOException] where it cannot be read, and [IllegalArgumentException] where it is not one. */
fun readLicences(assets: AssetManager): LicenceList = parseLicences(assets.open(LICENCE_LIST).use { it.readBytes().decodeToString() })

/** The text of [licence], as the build put it in the app. Throws [IOException] where it cannot be read. */
fun readLicenceText(assets: AssetManager, licence: Licence): String = assets.open(licence.textPath).use { it.readBytes().decodeToString() }

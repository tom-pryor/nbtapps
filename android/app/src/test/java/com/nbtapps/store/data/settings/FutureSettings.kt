// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.settings

/**
 * Options as a later agent might send them in `settings.get`, for a package whose manifest uses a later format of
 * `settings` behind `requires.agent`: what this app must show without an error. Each is the JSON text of the option's
 * object in the manifest, as `FakeSettings` takes them.
 */
object FutureSettings {
    const val PACKAGE = "org.nbtapp.future"

    /** A type this app does not know: read-only. */
    const val GLOW = """{"key":"GLOW","label":"Glow colour","description":"The colour of the dial's glow.","type":"colour","default":"#ff8800","applies":"now","advanced":false}"""

    /** An `applies` this app does not know: editable, and it takes effect "later". */
    const val VOLUME = """{"key":"VOLUME","label":"Volume","type":"int","min":0,"max":10,"default":5,"applies":"next-drive","advanced":false}"""

    /** A `characters` rule this app does not know: the app checks only for control characters. */
    const val CALL_SIGN = """{"key":"CALL_SIGN","label":"Call sign","type":"text","maxLength":20,"characters":"latin","applies":"now","advanced":false}"""

    /** A `maxLength` above today's 255. */
    const val BANNER = """{"key":"BANNER","label":"Banner","type":"text","maxLength":1000,"characters":"any","applies":"now","advanced":false}"""

    /** An `int` outside today's 32-bit limits. */
    const val OFFSET = """{"key":"OFFSET","label":"Offset","type":"int","min":-10000000000,"max":10000000000,"unit":"µs","applies":"now","advanced":true}"""

    /** A `select` of more than today's 16 values. */
    val STATION = """{"key":"STATION","label":"Station","type":"select","values":[""" +
        (1..20).joinToString(",") { """{"value":"s$it","label":"Station $it"}""" } +
        """],"default":"s20","applies":"now","advanced":true}"""

    /** A `bool` the agent says is in a state this app does not know: read-only. */
    const val MODE = """{"key":"MODE","label":"Mode","type":"bool","default":false,"applies":"reboot","advanced":false}"""

    /** A `select` whose value has a form a later version allows: read-only, as it breaks today's rules. */
    const val SOUND = """{"key":"SOUND","label":"Sound","type":"select","values":[{"value":"a b","label":"A B"},{"value":"c","label":"C"}],"applies":"now","advanced":false}"""

    val OPTIONS = listOf(GLOW, VOLUME, CALL_SIGN, BANNER, OFFSET, STATION, MODE, SOUND)

    /** The state the agent sends for [MODE]. */
    const val MODE_STATE = "locked"
}

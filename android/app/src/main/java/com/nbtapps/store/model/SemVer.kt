// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

/**
 * A package version (`major.minor.patch[-prerelease][+build]`), ordered by semver precedence.
 *
 * The form is the one `spec/manifest.schema.json` in the agent repo allows and nothing wider, so the app and
 * the head-unit agent agree on what a version is: three numbers of at most nine digits with no leading zero,
 * pre-release and build identifiers of `[0-9A-Za-z-]`, and at most 64 characters in all.
 */
data class SemVer(
    val major: Int,
    val minor: Int,
    val patch: Int,
    val preRelease: List<String> = emptyList(),
) : Comparable<SemVer> {

    override fun compareTo(other: SemVer): Int {
        compareValuesBy(this, other, SemVer::major, SemVer::minor, SemVer::patch).let { if (it != 0) return it }
        // A release ranks above any of its pre-releases.
        if (preRelease.isEmpty() || other.preRelease.isEmpty()) {
            return other.preRelease.size.compareTo(preRelease.size)
        }
        for ((mine, theirs) in preRelease.zip(other.preRelease)) {
            compareIdentifiers(mine, theirs).let { if (it != 0) return it }
        }
        return preRelease.size.compareTo(other.preRelease.size)
    }

    override fun toString(): String =
        "$major.$minor.$patch" + if (preRelease.isEmpty()) "" else preRelease.joinToString(".", prefix = "-")

    companion object {
        const val MAX_LENGTH = 64

        fun parseOrNull(text: String): SemVer? {
            if (text.length > MAX_LENGTH) return null
            val scanner = Scanner(text)
            val version = scanner.version() ?: return null
            return if (scanner.atEnd()) version else null
        }

        /** Numeric identifiers compare as numbers, whatever their length, and rank below alphanumeric ones. */
        private fun compareIdentifiers(a: String, b: String): Int {
            val aNumber = a.all { it in '0'..'9' }
            val bNumber = b.all { it in '0'..'9' }
            return when {
                aNumber && bNumber -> {
                    val x = a.trimStart('0')
                    val y = b.trimStart('0')
                    if (x.length != y.length) x.length.compareTo(y.length) else x.compareTo(y)
                }
                aNumber -> -1
                bNumber -> 1
                else -> a.compareTo(b)
            }
        }
    }

    /** Reads versions out of a text, for [SemVer] and [VersionRange] alike. */
    internal class Scanner(private val text: String) {
        var at = 0

        fun atEnd(): Boolean = at == text.length

        fun peek(): Char? = text.getOrNull(at)

        fun skip(prefix: String): Boolean {
            if (!text.startsWith(prefix, at)) return false
            at += prefix.length
            return true
        }

        /** A version starting here; stops at the first character that cannot continue it. */
        fun version(): SemVer? {
            val parts = IntArray(3)
            for (i in 0 until 3) {
                if (i > 0 && !skip(".")) return null
                parts[i] = number() ?: return null
            }
            val preRelease = if (skip("-")) identifiers() ?: return null else emptyList()
            if (skip("+")) identifiers() ?: return null
            return SemVer(parts[0], parts[1], parts[2], preRelease)
        }

        /** 0, or one to nine digits with no leading zero. */
        private fun number(): Int? {
            val start = at
            while (at < text.length && text[at] in '0'..'9') at++
            val length = at - start
            if (length == 0 || length > 9 || (length > 1 && text[start] == '0')) return null
            return text.substring(start, at).toInt()
        }

        /** One or more identifiers of `[0-9A-Za-z-]`, separated by dots. */
        private fun identifiers(): List<String>? {
            val out = ArrayList<String>()
            while (true) {
                val start = at
                while (at < text.length && isIdentifierChar(text[at])) at++
                if (at == start) return null
                out += text.substring(start, at)
                if (!skip(".")) return out
            }
        }

        private fun isIdentifierChar(c: Char) = c in '0'..'9' || c in 'A'..'Z' || c in 'a'..'z' || c == '-'
    }
}

/**
 * A version range: comparators separated by one space, all of which must hold, such as `>=2.0.0 <3.0.0`.
 * A comparator is a version with an optional operator in front (`>=`, `>`, `<=`, `<`, `=`); none means `=`.
 */
class VersionRange private constructor(private val comparators: List<Comparator>) {

    private enum class Op { EQUAL, LESS, LESS_EQUAL, GREATER, GREATER_EQUAL }

    private class Comparator(val op: Op, val version: SemVer)

    fun matches(version: SemVer): Boolean = comparators.all { comparator ->
        val order = version.compareTo(comparator.version)
        when (comparator.op) {
            Op.EQUAL -> order == 0
            Op.LESS -> order < 0
            Op.LESS_EQUAL -> order <= 0
            Op.GREATER -> order > 0
            Op.GREATER_EQUAL -> order >= 0
        }
    }

    companion object {
        const val MAX_LENGTH = 256

        fun parseOrNull(text: String): VersionRange? {
            if (text.length > MAX_LENGTH) return null
            val scanner = SemVer.Scanner(text)
            val comparators = ArrayList<Comparator>()
            while (true) {
                val op = when {
                    scanner.skip(">=") -> Op.GREATER_EQUAL
                    scanner.skip("<=") -> Op.LESS_EQUAL
                    scanner.skip(">") -> Op.GREATER
                    scanner.skip("<") -> Op.LESS
                    else -> {
                        scanner.skip("=")
                        Op.EQUAL
                    }
                }
                comparators += Comparator(op, scanner.version() ?: return null)
                if (scanner.atEnd()) return VersionRange(comparators)
                if (!scanner.skip(" ")) return null
            }
        }

        /**
         * Whether [version] is inside [range], for strings as a manifest or an index gives them. No range
         * matches every version; a version or a range that does not parse matches nothing.
         */
        fun matches(version: String, range: String?): Boolean {
            val parsed = SemVer.parseOrNull(version) ?: return false
            if (range.isNullOrEmpty()) return true
            return parseOrNull(range)?.matches(parsed) ?: false
        }
    }
}

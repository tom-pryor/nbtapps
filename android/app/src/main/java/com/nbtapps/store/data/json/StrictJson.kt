// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.json

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

/** The text is outside the strict JSON profile. The message is for logs, not for the user. */
class JsonProfileException(message: String) : Exception(message)

/**
 * Parses the strict JSON profile that `manifest.json` and the protocol's messages share:
 *
 *  - UTF-8 with no byte-order mark, and one object at the top level
 *  - no object has the same key twice
 *  - numbers are integers: no fraction, no exponent, and at most 2^53 - 1 in size
 *  - strings are valid UTF-8 with no unescaped control character and no unpaired surrogate escape
 *  - at most 16 levels of objects and arrays
 *
 * A general JSON library accepts texts outside this profile and settles them its own way (the last of two
 * equal keys wins, say). The package tool, the head-unit agent and this app must never read one manifest
 * three ways, so each refuses such a text whole. `spec/json-profile.vectors` in the agent repo holds the
 * cases all three are tested against.
 */
object StrictJson {
    const val MAX_DEPTH = 16
    const val MAX_INTEGER = (1L shl 53) - 1

    /** @throws JsonProfileException if [bytes] are not one object inside the profile. */
    fun parseObject(bytes: ByteArray): JsonObject {
        if (bytes.size >= 3 && bytes[0] == 0xEF.toByte() && bytes[1] == 0xBB.toByte() && bytes[2] == 0xBF.toByte()) {
            throw JsonProfileException("starts with a byte-order mark")
        }
        if (!Utf8.isValid(bytes)) throw JsonProfileException("is not valid UTF-8")
        val parser = Parser(String(bytes, Charsets.UTF_8))
        parser.skipWhitespace()
        if (parser.peek() != '{') throw JsonProfileException("does not start with an object")
        val root = parser.value(1) as JsonObject
        parser.skipWhitespace()
        if (!parser.atEnd()) throw JsonProfileException("has text after the object")
        return root
    }

    private class Parser(private val text: String) {
        private var at = 0

        fun atEnd(): Boolean = at >= text.length

        /** The next character, or NUL at the end: a raw NUL is never valid where this is asked. */
        fun peek(): Char = if (at < text.length) text[at] else '\u0000'

        fun skipWhitespace() {
            while (at < text.length && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) at++
        }

        fun value(depth: Int): JsonElement = when (val c = peek()) {
            '{' -> obj(depth)
            '[' -> array(depth)
            '"' -> JsonPrimitive(string())
            't' -> literal("true", JsonPrimitive(true))
            'f' -> literal("false", JsonPrimitive(false))
            'n' -> literal("null", JsonNull)
            else -> if (c == '-' || c in '0'..'9') number() else fail("has an unexpected character")
        }

        private fun obj(depth: Int): JsonObject {
            if (depth > MAX_DEPTH) fail("is nested deeper than $MAX_DEPTH levels")
            at++ // {
            val members = LinkedHashMap<String, JsonElement>()
            skipWhitespace()
            if (peek() == '}') {
                at++
                return JsonObject(members)
            }
            while (true) {
                skipWhitespace()
                if (peek() != '"') fail("has an object key that is not a string")
                val key = string()
                skipWhitespace()
                if (peek() != ':') fail("has a key with no colon after it")
                at++
                skipWhitespace()
                val value = value(depth + 1)
                if (members.put(key, value) != null) fail("has the key \"$key\" twice in one object")
                skipWhitespace()
                when (peek()) {
                    ',' -> at++
                    '}' -> {
                        at++
                        return JsonObject(members)
                    }
                    else -> fail("has an object that is not closed")
                }
            }
        }

        private fun array(depth: Int): JsonArray {
            if (depth > MAX_DEPTH) fail("is nested deeper than $MAX_DEPTH levels")
            at++ // [
            val items = ArrayList<JsonElement>()
            skipWhitespace()
            if (peek() == ']') {
                at++
                return JsonArray(items)
            }
            while (true) {
                skipWhitespace()
                items += value(depth + 1)
                skipWhitespace()
                when (peek()) {
                    ',' -> at++
                    ']' -> {
                        at++
                        return JsonArray(items)
                    }
                    else -> fail("has an array that is not closed")
                }
            }
        }

        private fun literal(word: String, value: JsonElement): JsonElement {
            if (!text.startsWith(word, at)) fail("has an unknown literal")
            at += word.length
            // "truefalse" and "falsey" must not pass as a literal with something after it.
            if (at < text.length && (text[at].isLetterOrDigit() || text[at] == '_')) fail("has an unknown literal")
            return value
        }

        private fun number(): JsonElement {
            val start = at
            if (peek() == '-') at++
            val digits = at
            while (at < text.length && text[at] in '0'..'9') at++
            val count = at - digits
            if (count == 0) fail("has a number with no digits")
            if (count > 1 && text[digits] == '0') fail("has a number with a leading zero")
            if (at < text.length && (text[at] == '.' || text[at] == 'e' || text[at] == 'E')) fail("has a number that is not an integer")
            // 2^53 - 1 has 16 digits, so anything longer is out of range before it is converted.
            if (count > 16) fail("has a number larger than 2^53 - 1")
            val value = text.substring(start, at).toLong()
            if (value > MAX_INTEGER || value < -MAX_INTEGER) fail("has a number larger than 2^53 - 1")
            return JsonPrimitive(value)
        }

        private fun string(): String {
            at++ // opening quote
            val out = StringBuilder()
            while (true) {
                if (at >= text.length) fail("has a string that is not closed")
                val c = text[at++]
                when {
                    c == '"' -> return out.toString()
                    c < ' ' -> fail("has a control character in a string")
                    c != '\\' -> out.append(c)
                    else -> escape(out)
                }
            }
        }

        private fun escape(out: StringBuilder) {
            if (at >= text.length) fail("has a string that is not closed")
            when (text[at++]) {
                '"' -> out.append('"')
                '\\' -> out.append('\\')
                '/' -> out.append('/')
                'b' -> out.append('\b')
                'f' -> out.append('\u000C')
                'n' -> out.append('\n')
                'r' -> out.append('\r')
                't' -> out.append('\t')
                'u' -> {
                    val unit = hex4()
                    when {
                        unit.isHighSurrogate() -> {
                            if (!text.startsWith("\\u", at)) fail("has a surrogate escape with no partner")
                            at += 2
                            val low = hex4()
                            if (!low.isLowSurrogate()) fail("has a surrogate escape with no partner")
                            out.append(unit).append(low)
                        }
                        unit.isLowSurrogate() -> fail("has a surrogate escape with no partner")
                        else -> out.append(unit)
                    }
                }
                else -> fail("has an unknown escape")
            }
        }

        private fun hex4(): Char {
            if (at + 4 > text.length) fail("has a \\u escape that is cut short")
            var value = 0
            repeat(4) {
                val digit = Character.digit(text[at++], 16)
                // Character.digit also takes digits of other scripts; only ASCII ones are hex here.
                if (digit < 0 || text[at - 1] > 'f') fail("has a \\u escape that is not four hex digits")
                value = value * 16 + digit
            }
            return value.toChar()
        }

        private fun fail(what: String): Nothing = throw JsonProfileException("$what (at character $at)")
    }
}

/** UTF-8 as RFC 3629 defines it: no overlong form, no surrogate, nothing above U+10FFFF. */
object Utf8 {
    fun isValid(bytes: ByteArray): Boolean {
        var i = 0
        while (i < bytes.size) {
            val b = bytes[i].toInt() and 0xFF
            val extra: Int
            val min: Int
            var code: Int
            when {
                b < 0x80 -> {
                    i++
                    continue
                }
                b in 0xC2..0xDF -> {
                    extra = 1
                    min = 0x80
                    code = b and 0x1F
                }
                b in 0xE0..0xEF -> {
                    extra = 2
                    min = 0x800
                    code = b and 0x0F
                }
                b in 0xF0..0xF4 -> {
                    extra = 3
                    min = 0x10000
                    code = b and 0x07
                }
                else -> return false
            }
            if (i + extra >= bytes.size) return false
            for (k in 1..extra) {
                val next = bytes[i + k].toInt() and 0xFF
                if (next and 0xC0 != 0x80) return false
                code = (code shl 6) or (next and 0x3F)
            }
            if (code < min || code > 0x10FFFF || code in 0xD800..0xDFFF) return false
            i += extra + 1
        }
        return true
    }

    /** The number of characters (code points) in [text]. */
    fun length(text: String): Int = text.codePointCount(0, text.length)
}

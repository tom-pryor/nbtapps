// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import java.io.IOException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The tries kept for the user to copy, what they are said to tell, and that no address gets out. */
class LinkDiagnosticsTest {

    private val address = Regex("([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}")

    private fun dial(outcome: LinkTry.Outcome, facts: TryFacts = TryFacts()) =
        LinkTry(at = 0, path = LinkTry.Path.DIAL, outcome = outcome, millis = 10, facts = facts)

    @Test
    fun `the last two hundred tries are kept, oldest first, with when they ended`() {
        var clock = 1_000L
        val diagnostics = SerialDiagnostics(wallClock = { clock++ })
        repeat(210) { diagnostics.record(LinkTry.Path.DIAL, LinkTry.Outcome.FAILED, millis = it.toLong(), failure = IOException("try $it")) }

        val kept = diagnostics.tries()
        assertEquals(200, kept.size)
        assertEquals("try 10", kept.first().message)
        assertEquals("try 209", kept.last().message)
        assertEquals(1_010L, kept.first().at)
        assertEquals("IOException", kept.first().error)
    }

    @Test
    fun `a try is one line, with what the phone could tell of it`() {
        val line = SerialDiagnostics().record(
            LinkTry.Path.DIAL, LinkTry.Outcome.FAILED, millis = 1_200, failure = IOException("read failed, socket might closed or timeout, read ret: -1"),
            facts = TryFacts(errorCode = 18, channel = 5, carConnected = true, bonded = true),
        ).describe()
        assertEquals(
            "dial failed after 1200 ms: IOException: read failed, socket might closed or timeout, read ret: -1 " +
                "(error code 18, channel 5, car connected, paired)",
            line,
        )
        assertEquals(
            "accept turned away after 0 ms (no channel, car not connected, not paired)",
            LinkTry(0, LinkTry.Path.ACCEPT, LinkTry.Outcome.TURNED_AWAY, 0, facts = TryFacts(channel = -1, carConnected = false, bonded = false)).describe(),
        )
        assertEquals("listen failed: IOException: Bluetooth is off", LinkTry(0, LinkTry.Path.LISTEN, LinkTry.Outcome.FAILED, error = "IOException", message = "Bluetooth is off").describe())
    }

    @Test
    fun `a connect that only listened says so, how long it listened, and whether the car's link came`() {
        assertEquals(
            "listen-only connect took the car's link after 12500 ms",
            LinkTry(0, LinkTry.Path.LISTEN_ONLY, LinkTry.Outcome.TAKEN, 12_500).describe(),
        )
        assertEquals(
            "listen-only connect had no link from the car after 40000 ms",
            LinkTry(0, LinkTry.Path.LISTEN_ONLY, LinkTry.Outcome.NO_LINK, 40_000).describe(),
        )
        // Only the phone's own dials say why a connect failed: a wait for the car tells nothing of the car.
        assertNull(causeOf(listOf(LinkTry(0, LinkTry.Path.LISTEN_ONLY, LinkTry.Outcome.NO_LINK, 40_000))))
    }

    @Test
    fun `the text to copy has the app, the head unit, the agent's serial status as it came, and every try`() {
        val diagnostics = SerialDiagnostics(about = "App 0.7.0, Android 16 (API 36), Pixel 9")
        diagnostics.record(LinkTry.Path.DIAL, LinkTry.Outcome.FAILED, millis = 700, failure = IOException("refused"))
        diagnostics.record(LinkTry.Path.ACCEPT, LinkTry.Outcome.UP, millis = 2)
        val serial = Json.parseToJsonElement("""{"listener":true,"dial":"on","counts":{"dials":4,"answeredOk":1}}""").jsonObject

        val text = diagnostics.report("BMW 12345", serial)

        val lines = text.lines()
        assertEquals("NBTApps Bluetooth diagnostics", lines[0])
        assertEquals("App 0.7.0, Android 16 (API 36), Pixel 9", lines[1])
        assertEquals("Head unit: BMW 12345", lines[2])
        assertTrue(text, text.contains("""{"listener":true,"dial":"on","counts":{"dials":4,"answeredOk":1}}"""))
        assertTrue(text, text.contains("The last 2 tries, oldest first:"))
        assertTrue(text, lines.any { it.endsWith(" dial failed after 700 ms: IOException: refused") })
        assertTrue(text, lines.any { it.endsWith(" accept up after 2 ms") })

        // Before any link in this run, and before any try.
        val empty = SerialDiagnostics().report(null, null)
        assertTrue(empty, empty.contains("not read in this run"))
        assertTrue(empty, empty.contains("No tries in this run"))
        assertFalse(empty, empty.contains("Head unit:"))
    }

    @Test
    fun `no Bluetooth address gets into the text to copy, wherever it came in`() {
        val diagnostics = SerialDiagnostics(about = "App 0.7.0 on 00:11:22:33:44:55")
        diagnostics.record(LinkTry.Path.DIAL, LinkTry.Outcome.FAILED, failure = IOException("connect to 00:11:22:33:44:55 failed"))
        diagnostics.record(LinkTry.Path.ACCEPT, LinkTry.Outcome.TURNED_AWAY, failure = IOException("from 66:77:88:99:aa:bb"))
        diagnostics.record(LinkTry.Path.LISTEN, LinkTry.Outcome.FAILED, failure = IOException("66-77-88-99-AA-BB is busy"))
        val serial = Json.parseToJsonElement("""{"listener":true,"phone":"00:11:22:33:44:55"}""").jsonObject

        val text = diagnostics.report("Car 66:77:88:99:AA:BB", serial)

        assertNull(text, address.find(text))
        assertTrue(text, text.contains("<address>"))
        assertEquals("BMW <address>.", withoutAddresses("BMW 00:11:22:33:44:55."))
        // What only looks like part of one is left alone.
        assertEquals("1.2.3 at 12:30:45", withoutAddresses("1.2.3 at 12:30:45"))
    }

    @Test
    fun `the cause is what the last try of the phone's that tells anything says`() {
        assertNull(causeOf(emptyList()))
        assertNull(causeOf(listOf(dial(LinkTry.Outcome.FAILED))))
        assertEquals(LinkCause.NO_SERVICE, causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1, carConnected = true)))))
        assertEquals(LinkCause.REFUSED, causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(channel = 5)))))
        assertEquals(LinkCause.NO_ANSWER, causeOf(listOf(dial(LinkTry.Outcome.NO_ANSWER))))
        assertEquals(LinkCause.NO_ANSWER, causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(errorCode = 14, channel = 5)))))
        // The last one that says anything counts; one that says nothing, and the car's links, are passed over.
        val tries = listOf(
            dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1, carConnected = true)),
            dial(LinkTry.Outcome.FAILED, TryFacts(channel = 5)),
            dial(LinkTry.Outcome.FAILED),
            LinkTry(0, LinkTry.Path.ACCEPT, LinkTry.Outcome.FAILED, facts = TryFacts(channel = -1)),
        )
        assertEquals(LinkCause.REFUSED, causeOf(tries))
    }

    @Test
    fun `no channel blames the agent only where the car is connected to the phone, and says nothing where that is not known`() {
        // The car answered, and has no service of the agent's.
        assertEquals(LinkCause.NO_SERVICE, causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1, carConnected = true, bonded = true)))))
        // The phone may never have reached the car: out of range, or its Bluetooth off.
        assertEquals(LinkCause.NOT_CONNECTED, causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1, carConnected = false, bonded = true)))))
        // The phone could not tell: that try says nothing, so the general text is used, or an earlier try's cause.
        assertNull(causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1)))))
        assertEquals(
            LinkCause.REFUSED,
            causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(channel = 5)), dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1)))),
        )
        // A try that heard nothing is not answered, connected or not.
        assertEquals(LinkCause.NO_ANSWER, causeOf(listOf(dial(LinkTry.Outcome.NO_ANSWER, TryFacts(channel = -1, carConnected = false)))))
        assertEquals(LinkCause.NO_ANSWER, causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(errorCode = 14, channel = -1, carConnected = true)))))
        // The last try that says anything counts.
        assertEquals(
            LinkCause.NOT_CONNECTED,
            causeOf(listOf(dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1, carConnected = true)), dial(LinkTry.Outcome.FAILED, TryFacts(channel = -1, carConnected = false)))),
        )
    }

    @Test
    fun `the refusal says the car will connect by itself only for an agent that dials`() {
        assertTrue(LinkCause.REFUSED.text(dials = true).contains("by itself"))
        assertTrue(LinkCause.REFUSED.text(dials = true).contains("pairing code"))
        assertFalse(LinkCause.REFUSED.text(dials = false).contains("by itself"))
        for (cause in LinkCause.entries) assertTrue(cause.text(dials = false).endsWith("."))
        for (cause in LinkCause.entries) assertTrue(cause.text(dials = true).endsWith("."))
    }

    @Test
    fun `a car that is not connected is put down to range or its Bluetooth, not to the agent`() {
        for (dials in listOf(false, true)) {
            val text = LinkCause.NOT_CONNECTED.text(dials)
            assertTrue(text, text.contains("out of range"))
            assertFalse(text, text.contains("agent"))
        }
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class EchoExchangeTest {

    /**
     * An echoing peer in memory: what is written to [output] can be read from [input]. The buffer is
     * small and bounded, like a real link, so a sender that doesn't read while it writes gets stuck.
     */
    private class Loopback(
        private val corruptAt: Int? = null,
        private val stopEchoingAfter: Int? = null,
        capacity: Int = 2 * 1024,
    ) {
        private val buffer = ArrayBlockingQueue<Byte>(capacity)
        private val written = AtomicInteger()

        val output = object : OutputStream() {
            override fun write(b: Int) {
                val offset = written.getAndIncrement()
                if (stopEchoingAfter != null && offset >= stopEchoingAfter) return
                buffer.put(if (offset == corruptAt) (b xor 0x55).toByte() else b.toByte())
            }
        }

        val input = object : InputStream() {
            private var delivered = 0

            override fun read(): Int {
                while (true) {
                    val next = buffer.poll(20, TimeUnit.MILLISECONDS)
                    if (next != null) {
                        delivered++
                        return next.toInt() and 0xFF
                    }
                    if (stopEchoingAfter != null && delivered >= stopEchoingAfter) return -1 // the peer hung up
                }
            }
        }
    }

    private fun exchange(link: Loopback) = EchoExchange(link.input, link.output)

    @Test(timeout = 20_000)
    fun `everything sent comes back and is reported intact`() {
        val progress = mutableListOf<Int>()

        val report = exchange(Loopback()).run(40_000) { progress += it }

        assertTrue(report.intact)
        assertEquals(40_000, report.bytesSent)
        assertEquals(40_000, report.bytesReceived)
        assertNull(report.firstMismatchAt)
        assertEquals(40_000, progress.last())
        assertEquals(progress.sorted(), progress)
        assertTrue(report.totalMillis >= report.roundTripMillis)
    }

    @Test(timeout = 20_000)
    fun `a test shorter than the first message still works`() {
        val report = exchange(Loopback()).run(5)

        assertTrue(report.intact)
        assertEquals(5, report.bytesReceived)
    }

    @Test(timeout = 20_000)
    fun `a changed byte is reported at its offset`() {
        val report = exchange(Loopback(corruptAt = 12_345)).run(20_000)

        assertFalse(report.intact)
        assertEquals(12_345, report.firstMismatchAt)
        assertEquals(20_000, report.bytesReceived)
    }

    @Test(timeout = 20_000)
    fun `a link that closes early fails with how far it got`() {
        val error = assertThrows(IOException::class.java) {
            exchange(Loopback(stopEchoingAfter = 1_000)).run(5_000)
        }

        assertTrue(error.message, error.message!!.contains("1000 of 5000"))
    }

    @Test
    fun `the pattern differs between neighbouring offsets and doesn't repeat every 256 bytes`() {
        assertFalse(EchoExchange.patternByte(0) == EchoExchange.patternByte(1))
        assertFalse(EchoExchange.patternByte(10) == EchoExchange.patternByte(10 + 256))
    }

    @Test
    fun `sending nothing is refused`() {
        assertThrows(IllegalArgumentException::class.java) { exchange(Loopback()).run(0) }
    }
}

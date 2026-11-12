// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class HeadUnitFormTest {

    private val valid = HeadUnitForm(name = "Car", host = "192.168.1.1", port = "22")
    private val car = PairedDevice("BMW 12345", "00:11:22:33:44:55")
    private val headphones = PairedDevice("Headphones", "66:77:88:99:AA:BB")

    @Test
    fun `valid form has no errors`() {
        assertTrue(valid.isValid)
        assertNull(valid.nameError)
        assertNull(valid.linkError)
        assertNull(valid.hostError)
        assertNull(valid.portError)
    }

    @Test
    fun `a form with no name and no way to reach the head unit is rejected for both`() {
        val form = HeadUnitForm(name = " ", host = "", port = "")
        assertFalse(form.isValid)
        assertNotNull(form.nameError)
        assertNotNull(form.linkError)
        // The address fields are not at fault: the car could as well be picked.
        assertNull(form.hostError)
        assertNull(form.portError)
    }

    @Test
    fun `an address that was begun must be whole`() {
        val hostOnly = HeadUnitForm(name = "Car", host = "192.168.1.1")
        assertFalse(hostOnly.isValid)
        assertNull(hostOnly.linkError)
        assertNull(hostOnly.hostError)
        assertNotNull(hostOnly.portError)

        val portOnly = HeadUnitForm(name = "Car", port = "22")
        assertFalse(portOnly.isValid)
        assertNotNull(portOnly.hostError)
        assertNull(portOnly.portError)

        // And picking the car does not excuse half an address.
        assertFalse(hostOnly.copy(bluetooth = car).isValid)
    }

    @Test
    fun `host with inner whitespace is rejected`() {
        assertNotNull(valid.copy(host = "192.168 .1.1").hostError)
    }

    @Test
    fun `port must be in range`() {
        assertNotNull(valid.copy(port = "0").portError)
        assertNotNull(valid.copy(port = "65536").portError)
        assertNotNull(valid.copy(port = "abc").portError)
        assertNull(valid.copy(port = "65535").portError)
    }

    @Test
    fun `toHeadUnit trims input`() {
        val unit = HeadUnitForm(" Car ", " 10.0.0.1 ", " 8080 ").toHeadUnit("id")
        assertEquals("Car", unit.name)
        assertEquals(listOf(TcpEndpoint("10.0.0.1", 8080)), unit.endpoints)
        assertNull(unit.headUnitId)
    }

    @Test
    fun `the car picked among the paired devices is enough, and becomes a Bluetooth serial endpoint`() {
        val form = HeadUnitForm(name = "Car", bluetooth = car)
        assertTrue(form.isValid)
        assertEquals(listOf(SerialEndpoint("00:11:22:33:44:55", "BMW 12345")), form.toHeadUnit("id").endpoints)
    }

    @Test
    fun `a head unit with both has both, the address first`() {
        val unit = valid.copy(bluetooth = car).toHeadUnit("id")
        assertEquals(
            listOf(TcpEndpoint("192.168.1.1", 22), SerialEndpoint("00:11:22:33:44:55", "BMW 12345")),
            unit.endpoints,
        )
    }

    @Test
    fun `where the build has no TCP link, the car is the only way and an address is ignored`() {
        val serialOnly = HeadUnitForm(name = "Car", host = "192.168.1.1", port = "0", takesAddress = false)
        assertFalse(serialOnly.isValid)
        assertEquals("Pick the car under Bluetooth", serialOnly.linkError)
        // Not even half an address or a bad port counts against it: the fields are not on the screen.
        assertNull(serialOnly.hostError)
        assertNull(serialOnly.portError)

        val unit = serialOnly.copy(bluetooth = car).toHeadUnit("id")
        assertEquals(listOf(SerialEndpoint("00:11:22:33:44:55", "BMW 12345")), unit.endpoints)
    }

    @Test
    fun `picking the car names a head unit that has no name yet, and leaves a name that was entered`() {
        assertEquals("BMW 12345", HeadUnitForm().withDevice(car).name)
        assertEquals("BMW 12345", HeadUnitForm(name = "  ").withDevice(car).name)
        assertEquals("My car", HeadUnitForm(name = "My car").withDevice(car).name)
        assertEquals(car, HeadUnitForm(name = "My car").withDevice(car).bluetooth)
        // Picking none again takes the device and leaves the name.
        val unpicked = HeadUnitForm().withDevice(car).withDevice(null)
        assertNull(unpicked.bluetooth)
        assertEquals("BMW 12345", unpicked.name)
    }

    @Test
    fun `a picked device that is no longer among the paired ones is not picked any more`() {
        val form = HeadUnitForm(name = "Car", bluetooth = car)
        assertSame(form, form.forDevices(listOf(headphones, car)))
        assertNull(form.forDevices(listOf(headphones)).bluetooth)
        assertNull(form.forDevices(emptyList()).bluetooth)
        assertFalse(form.forDevices(emptyList()).isValid)
        assertSame(valid, valid.forDevices(emptyList()))
    }
}

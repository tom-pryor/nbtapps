// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.job

import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.model.JobSnapshot
import com.nbtapps.store.model.JobState
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** When the job screen offers to connect from the phone: [offersConnectFromPhone]. */
class JobConnectFromPhoneTest {

    private fun job(on: String = "car") =
        JobView("1011121314151617", on, plan = null, job = JobSnapshot("1011121314151617", JobState.FETCHING), linkLost = true)

    @Test
    fun `a connect to the job's head unit that only waited for the car's link offers it, and nothing else does`() {
        assertTrue(offersConnectFromPhone(job(), ConnectionState.Failed("car", "No link came", listenedOnly = true)))
        assertFalse(offersConnectFromPhone(job(), ConnectionState.Failed("car", "No route to the car")))
        // Another head unit's failure, no job, and no failure at all.
        assertFalse(offersConnectFromPhone(job("van"), ConnectionState.Failed("car", "No link came", listenedOnly = true)))
        assertFalse(offersConnectFromPhone(null, ConnectionState.Failed("car", "No link came", listenedOnly = true)))
        assertFalse(offersConnectFromPhone(job(), ConnectionState.Disconnected))
        assertFalse(offersConnectFromPhone(job(), ConnectionState.Connecting("car")))
    }
}

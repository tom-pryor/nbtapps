// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.update

import android.content.pm.PackageInstaller
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** What a release is to the copy that runs ([appUpdate]), and what a download has to be ([apkProblem]). */
class AppUpdateTest {

    @Test
    fun `a release with a higher version code is an update`() {
        val release = release("0.2.0", 200)
        assertEquals(AppUpdateOffer.Available(release), appUpdate(release, RUNNING))
    }

    @Test
    fun `no release, the same one and an older one are nothing`() {
        assertNull(appUpdate(null, RUNNING))
        assertNull(appUpdate(release("0.1.0", 100), RUNNING))
        assertNull(appUpdate(release("0.0.9", 9), RUNNING))
    }

    @Test
    fun `only the version code says what is newer`() {
        // A name that reads lower with a code that is higher is an update, as it is to Android.
        val release = release("0.0.1", 101)
        assertEquals(AppUpdateOffer.Available(release), appUpdate(release, RUNNING))
    }

    @Test
    fun `a copy signed with another key cannot take a release`() {
        val release = release()
        val blocked = AppUpdateOffer.Blocked(release, AppUpdateOffer.Reason.OTHER_KEY)
        assertEquals(blocked, appUpdate(release, RUNNING.copy(certSha256 = OTHER_CERT)))
        // Nor one whose key Android would not name: it cannot be known to be the release's.
        assertEquals(blocked, appUpdate(release, RUNNING.copy(certSha256 = null)))
        // The key comes first: such a copy is not told to get a newer Android.
        assertEquals(blocked, appUpdate(release, RUNNING.copy(certSha256 = OTHER_CERT, sdk = 30)))
        // And an older release is nothing to it, as to any copy.
        assertNull(appUpdate(release("0.1.0", 100), RUNNING.copy(certSha256 = OTHER_CERT)))
    }

    @Test
    fun `a release needs the Android it names`() {
        val release = release().copy(minSdk = 35)
        assertEquals(AppUpdateOffer.Blocked(release, AppUpdateOffer.Reason.ANDROID_TOO_OLD), appUpdate(release, RUNNING))
        assertEquals(AppUpdateOffer.Available(release), appUpdate(release, RUNNING.copy(sdk = 35)))
    }

    @Test
    fun `a download has to be this app in the version named and with this copy's key`() {
        val release = release("0.2.0", 200)
        val good = ApkFacts(APP_PACKAGE, 200, RELEASE_CERT)
        assertNull(apkProblem(good, release, RUNNING))
        assertEquals("The download isn't an Android app.", apkProblem(null, release, RUNNING))
        assertEquals("The download is another app (org.other.app).", apkProblem(good.copy(packageName = "org.other.app"), release, RUNNING))
        assertEquals(
            "The download is version code 199, not the 200 the catalogue names.",
            apkProblem(good.copy(versionCode = 199), release, RUNNING),
        )
        assertEquals("The download is signed with another key than this app.", apkProblem(good.copy(certSha256 = OTHER_CERT), release, RUNNING))
    }

    @Test
    fun `a certificate that could not be read is left to Android`() {
        assertNull(apkProblem(ApkFacts(APP_PACKAGE, 200, certSha256 = null), release("0.2.0", 200), RUNNING))
    }

    @Test
    fun `Android's refusals are put in words, with its own beside them`() {
        assertEquals("The update was cancelled.", installFailure(PackageInstaller.STATUS_FAILURE_ABORTED, "User rejected permissions"))
        assertEquals(
            "The update doesn't fit the app that is installed. (INSTALL_FAILED_UPDATE_INCOMPATIBLE)",
            installFailure(PackageInstaller.STATUS_FAILURE_CONFLICT, "INSTALL_FAILED_UPDATE_INCOMPATIBLE"),
        )
        assertEquals("The phone has no room for the update.", installFailure(PackageInstaller.STATUS_FAILURE_STORAGE, null))
        assertEquals("Android didn't install the update. (odd)", installFailure(PackageInstaller.STATUS_FAILURE, "odd"))
        assertEquals("Android didn't install the update.", installFailure(99, " "))
    }
}

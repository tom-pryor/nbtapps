// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.di

import com.nbtapps.store.data.catalogue.CatalogueSource
import com.nbtapps.store.data.catalogue.OfficialCatalogue
import com.nbtapps.store.data.link.BluetoothSerialLinkConnector
import com.nbtapps.store.data.link.LinkConnector
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

/**
 * The release build's one catalogue is the official one, fetched over HTTPS and checked against the official
 * keys. The debug build's sample catalogue and its test keys are not in this build.
 *
 * Its one link to a head unit is Bluetooth serial, which the phone's pairing with the car encrypts and on which
 * the agent asks for `auth`. The debug build's plain TCP link is not in this build.
 */
@Module
@InstallIn(SingletonComponent::class)
object BuildTypeModule {
    @Provides
    @Singleton
    fun provideCatalogueSource(): CatalogueSource = OfficialCatalogue.source()

    @Provides
    @Singleton
    fun provideLinkConnector(serial: BluetoothSerialLinkConnector): LinkConnector = serial
}

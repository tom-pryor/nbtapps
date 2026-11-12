// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.di

import com.nbtapps.store.data.catalogue.AssetCatalogueSource
import com.nbtapps.store.data.catalogue.CatalogueSource
import com.nbtapps.store.data.link.BluetoothSerialLinkConnector
import com.nbtapps.store.data.link.DebugTcpLinkConnector
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.LinkConnectors
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

/**
 * What only the debug build has: the sample catalogue, signed with test keys, and a plain TCP link with no
 * encryption, for the host build of the agent. Both are for the test bench only. Bluetooth serial is there
 * beside the TCP link, as in every build.
 */
@Module
@InstallIn(SingletonComponent::class)
object BuildTypeModule {
    @Provides
    @Singleton
    fun provideCatalogueSource(source: AssetCatalogueSource): CatalogueSource = source

    @Provides
    @Singleton
    fun provideLinkConnector(serial: BluetoothSerialLinkConnector): LinkConnector =
        LinkConnectors(listOf(serial, DebugTcpLinkConnector()))
}

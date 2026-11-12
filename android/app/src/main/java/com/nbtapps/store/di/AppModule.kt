// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.di

import android.content.Context
import android.os.Build
import androidx.datastore.dataStoreFile
import com.nbtapps.store.data.AndroidPhoneBluetooth
import com.nbtapps.store.data.BluetoothSppEchoClient
import com.nbtapps.store.data.DataStoreHeadUnitRepository
import com.nbtapps.store.data.FilePhoneIdentity
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.KeystorePairingKeys
import com.nbtapps.store.data.PairingKeys
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.SppEchoClient
import com.nbtapps.store.data.headUnitStore
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueSource
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.jobs.JobKeeper
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.AndroidSerialRadio
import com.nbtapps.store.data.link.BluetoothSerialLinkConnector
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.update.ApkInstaller
import com.nbtapps.store.data.update.AppUpdater
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.data.update.PackageManagerApkInspector
import com.nbtapps.store.data.update.SessionApkInstaller
import com.nbtapps.store.data.update.installedApp
import com.nbtapps.store.service.ServiceJobKeeper
import dagger.Binds
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import java.io.File
import javax.inject.Qualifier
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/** A scope that lives as long as the process: for work that must go on when the screen that started it is left. */
@Qualifier
@Retention(AnnotationRetention.BINARY)
annotation class ApplicationScope

@Module
@InstallIn(SingletonComponent::class)
abstract class AppModule {

    @Binds
    abstract fun bindSppEchoClient(impl: BluetoothSppEchoClient): SppEchoClient

    @Binds
    abstract fun bindPhoneBluetooth(impl: AndroidPhoneBluetooth): PhoneBluetooth

    @Binds
    abstract fun bindCatalogueRepository(impl: DefaultCatalogueRepository): CatalogueRepository

    @Binds
    abstract fun bindJobKeeper(impl: ServiceJobKeeper): JobKeeper

    @Binds
    abstract fun bindApkInstaller(impl: SessionApkInstaller): ApkInstaller

    companion object {
        @Provides
        @Singleton
        @ApplicationScope
        fun provideApplicationScope(): CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

        @Provides
        @Singleton
        fun provideHeadUnitRepository(@ApplicationContext context: Context): HeadUnitRepository =
            DataStoreHeadUnitRepository(
                headUnitStore(context.dataStoreFile("head-units.json"), CoroutineScope(SupervisorJob() + Dispatchers.IO)),
            )

        @Provides
        @Singleton
        fun providePhoneIdentity(@ApplicationContext context: Context): PhoneIdentity =
            FilePhoneIdentity(File(context.filesDir, "phone-id"), name = Build.MODEL ?: "Android phone")

        /** Downloaded archives live in the files folder, not the cache: they must survive until the phone is in the car. */
        @Provides
        @Singleton
        fun providePackageStore(@ApplicationContext context: Context, @ApplicationScope scope: CoroutineScope): PackageStore =
            PackageStore(File(context.filesDir, "packages")).also { store -> scope.launch { store.load() } }

        @Provides
        @Singleton
        fun providePackageDownloader(
            source: CatalogueSource,
            store: PackageStore,
            @ApplicationScope scope: CoroutineScope,
        ): PackageDownloader = PackageDownloader(source, store, scope)

        /** Read once: the copy that runs does not change while it runs. */
        @Provides
        @Singleton
        fun provideInstalledApp(@ApplicationContext context: Context): InstalledApp = installedApp(context)

        /** The app's update of itself. Its download is in the cache folder: it is of no use once it is installed. */
        @Provides
        @Singleton
        fun provideAppUpdater(
            @ApplicationContext context: Context,
            source: CatalogueSource,
            installed: InstalledApp,
            installer: ApkInstaller,
            jobs: JobRepository,
            @ApplicationScope scope: CoroutineScope,
        ): AppUpdater = AppUpdater(
            source = source,
            installed = installed,
            dir = File(context.cacheDir, "update"),
            inspector = PackageManagerApkInspector(context),
            installer = installer,
            busy = { jobs.job.value?.needsPhone == true },
            scope = scope,
        )

        /** The keys head units issued this phone when it was paired with them, in the Android Keystore. */
        @Provides
        @Singleton
        fun providePairingKeys(): PairingKeys = KeystorePairingKeys()

        /** Every build has it. Who may use the link is settled over it, by `auth` with a pairing key. */
        @Provides
        @Singleton
        fun provideSerialLinkConnector(radio: AndroidSerialRadio): BluetoothSerialLinkConnector = BluetoothSerialLinkConnector(radio)

        @Provides
        @Singleton
        fun provideJobRepository(
            headUnits: HeadUnitRepository,
            connector: LinkConnector,
            identity: PhoneIdentity,
            keys: PairingKeys,
            store: PackageStore,
            keeper: JobKeeper,
            @ApplicationScope scope: CoroutineScope,
        ): JobRepository = JobRepository(headUnits, connector, identity, keys, store, keeper, scope)
    }
}

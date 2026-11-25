// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.di

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.SystemClock
import androidx.datastore.dataStoreFile
import com.nbtapps.store.data.ActivityForeground
import com.nbtapps.store.data.AndroidPhoneBluetooth
import com.nbtapps.store.data.AppForeground
import com.nbtapps.store.data.BluetoothSppEchoClient
import com.nbtapps.store.data.DataStoreHeadUnitRepository
import com.nbtapps.store.data.DataStorePrereleaseRepository
import com.nbtapps.store.data.DataStoreSetupRepository
import com.nbtapps.store.data.FilePhoneIdentity
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.KeystorePairingKeys
import com.nbtapps.store.data.PairingKeys
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.PrereleaseRepository
import com.nbtapps.store.data.SetupRepository
import com.nbtapps.store.data.SppEchoClient
import com.nbtapps.store.data.headUnitStore
import com.nbtapps.store.data.prereleaseStore
import com.nbtapps.store.data.setupStore
import com.nbtapps.store.data.catalogue.CatalogueKeeper
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueSource
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.JobKeeper
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.AndroidLinkLog
import com.nbtapps.store.data.link.AndroidSerialRadio
import com.nbtapps.store.data.link.BluetoothSerialLinkConnector
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.SerialDiagnostics
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.ssh.ACCESS_LOCAL_NETWORK
import com.nbtapps.store.data.ssh.AuthorizedClients
import com.nbtapps.store.data.ssh.DataStoreAuthorizedClients
import com.nbtapps.store.data.ssh.JobPortLink
import com.nbtapps.store.data.ssh.KeystoreSshIdentity
import com.nbtapps.store.data.ssh.SshChannel
import com.nbtapps.store.data.ssh.SshIdentity
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.ssh.authorizedClientsStore
import com.nbtapps.store.data.ssh.localNetworkBlocked
import com.nbtapps.store.data.terminal.JobTerminalLink
import com.nbtapps.store.data.terminal.SshShellOpener
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.update.ApkInstaller
import com.nbtapps.store.data.update.AppUpdater
import com.nbtapps.store.ui.headunits.ConnectGate
import com.nbtapps.store.ui.headunits.ConnectGuard
import com.nbtapps.store.ui.terminal.CommandHistory
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.data.update.PackageManagerApkInspector
import com.nbtapps.store.data.update.SessionApkInstaller
import com.nbtapps.store.data.update.installedApp
import com.nbtapps.store.data.whatsnew.WhatsNewRepository
import com.nbtapps.store.data.whatsnew.whatsNewStore
import com.nbtapps.store.service.ServiceJobKeeper
import com.nbtapps.store.ui.pairrequest.ElapsedClock
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
    abstract fun bindJobKeeper(impl: ServiceJobKeeper): JobKeeper

    @Binds
    abstract fun bindApkInstaller(impl: SessionApkInstaller): ApkInstaller

    @Binds
    abstract fun bindAppForeground(impl: ActivityForeground): AppForeground

    @Binds
    abstract fun bindConnectGuard(impl: ConnectGate): ConnectGuard

    companion object {
        @Provides
        @Singleton
        @ApplicationScope
        fun provideApplicationScope(): CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

        /** Whether one of the app's activities is started, as the application reports it. */
        @Provides
        @Singleton
        fun provideActivityForeground(@ApplicationScope scope: CoroutineScope): ActivityForeground = ActivityForeground(scope)

        /** Since the phone started, its sleep included: the same in every process of the app. */
        @Provides
        fun provideElapsedClock(): ElapsedClock = ElapsedClock(SystemClock::elapsedRealtime)

        /** The catalogue, in memory. Its fetches go on when the screen that asked for one is left. */
        @Provides
        @Singleton
        fun provideCatalogueRepository(
            source: CatalogueSource,
            @ApplicationScope scope: CoroutineScope,
            clock: ElapsedClock,
        ): CatalogueRepository = DefaultCatalogueRepository(
            source,
            scope,
            // The catalogue's age counts the time the phone slept.
            clock = clock::millis,
        )

        /** Has the catalogue fetched again as the app comes to the front, once it is old. The application starts it. */
        @Provides
        @Singleton
        fun provideCatalogueKeeper(
            catalogue: CatalogueRepository,
            foreground: AppForeground,
            @ApplicationScope scope: CoroutineScope,
        ): CatalogueKeeper = CatalogueKeeper(catalogue, foreground, scope)

        @Provides
        @Singleton
        fun provideHeadUnitRepository(@ApplicationContext context: Context): HeadUnitRepository =
            DataStoreHeadUnitRepository(
                headUnitStore(context.dataStoreFile("head-units.json"), CoroutineScope(SupervisorJob() + Dispatchers.IO)),
            )

        /** Which packages the user has switched on for pre-release versions, for every head unit. */
        @Provides
        @Singleton
        fun providePrereleaseRepository(@ApplicationContext context: Context): PrereleaseRepository =
            DataStorePrereleaseRepository(
                prereleaseStore(context.dataStoreFile("prerelease-packages.json"), CoroutineScope(SupervisorJob() + Dispatchers.IO)),
            )

        /** Whether the user has skipped the set-up for a new user. */
        @Provides
        @Singleton
        fun provideSetupRepository(@ApplicationContext context: Context): SetupRepository =
            DataStoreSetupRepository(setupStore(context.dataStoreFile("setup.json"), CoroutineScope(SupervisorJob() + Dispatchers.IO)))

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

        /** What is new in the version that runs, shown once after an update, and what of it has been seen. */
        @Provides
        @Singleton
        fun provideWhatsNewRepository(
            @ApplicationContext context: Context,
            catalogue: CatalogueRepository,
            installed: InstalledApp,
            @ApplicationScope scope: CoroutineScope,
        ): WhatsNewRepository = WhatsNewRepository(
            catalogue,
            whatsNewStore(context.dataStoreFile("whats-new.json"), CoroutineScope(SupervisorJob() + Dispatchers.IO)),
            installed,
            scope,
        )

        /** The app's update of itself. Its download is in the cache folder: it is of no use once it is installed. */
        @Provides
        @Singleton
        fun provideAppUpdater(
            @ApplicationContext context: Context,
            source: CatalogueSource,
            installed: InstalledApp,
            installer: ApkInstaller,
            jobs: JobRepository,
            terminal: TerminalRepository,
            port: SshPort,
            whatsNew: WhatsNewRepository,
            @ApplicationScope scope: CoroutineScope,
        ): AppUpdater = AppUpdater(
            source = source,
            installed = installed,
            dir = File(context.cacheDir, "update"),
            inspector = PackageManagerApkInspector(context),
            installer = installer,
            busy = { jobs.job.value?.needsPhone == true },
            scope = scope,
            terminalOpen = { terminal.state.value.active },
            portOn = { port.state.value.active },
            saveNotes = whatsNew::saveNotes,
        )

        /** The SSH port for other apps: its connections go on when the screen, or the app, is left. */
        @Provides
        @Singleton
        fun provideSshPort(
            @ApplicationContext context: Context,
            jobs: JobRepository,
            clients: AuthorizedClients,
            keeper: JobKeeper,
            @ApplicationScope scope: CoroutineScope,
            clock: ElapsedClock,
        ): SshPort = SshPort(
            JobPortLink(jobs),
            SshChannel,
            clients,
            keeper,
            scope,
            // The idle limit counts the time the phone slept.
            clock = clock::millis,
            otherDevicesBlocked = {
                localNetworkBlocked(
                    Build.VERSION.SDK_INT,
                    context.applicationInfo.targetSdkVersion,
                    context.checkSelfPermission(ACCESS_LOCAL_NETWORK) == PackageManager.PERMISSION_GRANTED,
                )
            },
        )

        /** The lines sent from the terminal's field, for Up and Down: in memory only, for as long as the process lives. */
        @Provides
        @Singleton
        fun provideCommandHistory(): CommandHistory = CommandHistory()

        /** The keys head units issued this phone when it was paired with them, in the Android Keystore. */
        @Provides
        @Singleton
        fun providePairingKeys(): PairingKeys = KeystorePairingKeys()

        /** The app's own SSH key, kept under a key of the Android Keystore, as the pairing keys are kept. */
        @Provides
        @Singleton
        fun provideSshIdentity(@ApplicationContext context: Context): SshIdentity = KeystoreSshIdentity(File(context.filesDir, "ssh-key"))

        /** The public keys of the other SSH clients the user added. */
        @Provides
        @Singleton
        fun provideAuthorizedClients(@ApplicationContext context: Context): AuthorizedClients =
            DataStoreAuthorizedClients(
                authorizedClientsStore(context.dataStoreFile("ssh-clients.json"), CoroutineScope(SupervisorJob() + Dispatchers.IO)),
            )

        /** The last tries at a Bluetooth serial link, in memory only, for the user to copy where a connect failed. */
        @Provides
        @Singleton
        fun provideSerialDiagnostics(installed: InstalledApp): SerialDiagnostics = SerialDiagnostics(
            about = "App ${installed.versionName}, Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}), ${Build.MODEL ?: "unknown phone"}",
        )

        /** Every build has it. Who may use the link is settled over it, by `auth` with a pairing key. */
        @Provides
        @Singleton
        fun provideSerialLinkConnector(radio: AndroidSerialRadio, clock: ElapsedClock, diagnostics: SerialDiagnostics): BluetoothSerialLinkConnector =
            BluetoothSerialLinkConnector(radio, now = clock::millis, diagnostics = diagnostics, log = AndroidLinkLog)

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
            @ApplicationContext context: Context,
            foreground: AppForeground,
            clock: ElapsedClock,
            bluetooth: PhoneBluetooth,
        ): JobRepository = JobRepository(
            headUnits, connector, identity, keys, store, keeper, scope,
            // A link's silence counts the time the phone slept and the time the process was frozen.
            clock = clock::millis,
            shareDir = File(context.cacheDir, "share"),
            foreground = foreground,
            // Read now, which also begins following Bluetooth being switched on and off.
            bluetoothOn = bluetooth.also { it.refreshConnected() }.on,
            carsHolding = bluetooth.connected,
            mediaAudio = bluetooth::mediaAudio,
            log = AndroidLinkLog,
        )

        /** The terminal on a head unit: its shell goes on when the screen, or the app, is left. */
        @Provides
        @Singleton
        fun provideTerminalRepository(
            jobs: JobRepository,
            identity: SshIdentity,
            keeper: JobKeeper,
            @ApplicationScope scope: CoroutineScope,
            clock: ElapsedClock,
        ): TerminalRepository = TerminalRepository(
            JobTerminalLink(jobs),
            SshShellOpener(identity),
            keeper,
            scope,
            // The idle limit counts the time the phone slept, as the head unit's server does.
            clock = clock::millis,
        )

        /** Setting a head unit up to start the agent with the car, which goes on when the pairing screen is left. */
        @Provides
        @Singleton
        fun provideAutostartSetup(
            jobs: JobRepository,
            catalogue: CatalogueRepository,
            store: PackageStore,
            downloader: PackageDownloader,
            prereleases: PrereleaseRepository,
            @ApplicationScope scope: CoroutineScope,
        ): AutostartSetup = AutostartSetup(jobs, catalogue, store, downloader, prereleases, scope)
    }
}

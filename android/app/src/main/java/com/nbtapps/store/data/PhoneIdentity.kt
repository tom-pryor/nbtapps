// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.link.AgentClient
import java.io.File
import java.io.IOException
import java.security.SecureRandom
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/** What this phone says of itself in `hello`, so a head unit recognises it across links. */
interface PhoneIdentity {
    /** 32 hex characters, made from 16 random bytes the first time it is asked for and kept from then on. */
    suspend fun id(): String

    /** A name for the car's screen. */
    val name: String
}

class FilePhoneIdentity(
    private val file: File,
    override val name: String,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) : PhoneIdentity {
    private val lock = Mutex()
    private var cached: String? = null

    override suspend fun id(): String = lock.withLock {
        cached ?: withContext(io) {
            val stored = try {
                file.readText().trim().takeIf(AgentClient::isId)
            } catch (e: IOException) {
                null
            }
            stored ?: Hex.encode(ByteArray(16).also { SecureRandom().nextBytes(it) }).also {
                file.parentFile?.mkdirs()
                file.writeText(it)
            }
        }.also { cached = it }
    }
}

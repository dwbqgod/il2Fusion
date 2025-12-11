package com.tools.il2fusion.config

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Mediates data access between the UI and HookConfigStore to keep logic centralized.
 */
class HookConfigRepository {

    /**
     * Loads stored target method list and dump mode flag from the shared content provider.
     */
    suspend fun loadConfig(context: Context): HookConfigPayload = withContext(Dispatchers.IO) {
        val savedTargets = HookConfigStore.loadTargetsForApp(context)
        val dumpMode = HookConfigStore.loadDumpModeForApp(context)
        HookConfigPayload(savedTargets, dumpMode)
    }

    /**
     * Persists the dump mode flag through the content provider.
     */
    suspend fun saveDumpMode(context: Context, enabled: Boolean) = withContext(Dispatchers.IO) {
        HookConfigStore.saveDumpMode(context, enabled)
    }

    /**
     * Persists the target method list through the content provider.
     */
    suspend fun saveTargets(context: Context, targets: List<String>) = withContext(Dispatchers.IO) {
        HookConfigStore.saveTargets(context, targets)
    }
}

/**
 * Represents stored configuration values used by both the UI and native hook.
 */
data class HookConfigPayload(
    val targets: List<String>,
    val dumpModeEnabled: Boolean
)

package com.tools.il2fusion.config

import android.content.Context
import android.util.Log
import android.content.ContentValues
import com.tools.il2fusion.config.ConfigContentProvider.Companion.CONTENT_URI
import com.tools.il2fusion.config.ConfigContentProvider.Companion.KEY_TARGETS
import com.tools.il2fusion.config.ConfigContentProvider.Companion.KEY_DUMP_MODE

object HookConfigStore {
    private const val TAG = "[il2Fusion]"
    fun saveTargets(ctx: Context, targets: List<String>) {
        val storeCtx = ctx.applicationContext ?: ctx
        val text = targets.joinToString(separator = ",")
        val values = ContentValues().apply {
            put("key", KEY_TARGETS)
            put("value", text)
        }
        storeCtx.contentResolver.insert(CONTENT_URI, values)
        Log.i(TAG, "saveTargets(): stored ${targets.size} items -> $text via provider")
    }

    fun saveDumpMode(ctx: Context, enabled: Boolean) {
        val storeCtx = ctx.applicationContext ?: ctx
        val values = ContentValues().apply {
            put("key", KEY_DUMP_MODE)
            put("value", if (enabled) "1" else "0")
        }
        storeCtx.contentResolver.insert(CONTENT_URI, values)
        Log.i(TAG, "saveDumpMode(): $enabled")
    }

    fun loadTargetsForApp(ctx: Context): List<String> {
        val storeCtx = ctx.applicationContext ?: ctx
        return queryTargets(storeCtx)
    }

    fun loadTargetsForHook(ctx: Context): List<String> {
        return queryTargets(ctx)
    }

    fun loadDumpModeForApp(ctx: Context): Boolean {
        val storeCtx = ctx.applicationContext ?: ctx
        return queryDumpMode(storeCtx)
    }

    fun loadDumpModeForHook(ctx: Context): Boolean {
        return queryDumpMode(ctx)
    }

    fun markHookedPackage(ctx: Context, pkg: String): Set<String> {
        // SharedPreferences 跨进程不可读，这里仅返回当前包用于日志提示
        return setOf(pkg)
    }

    fun enabledPackages(ctx: Context): Set<String> {
        return emptySet()
    }

    private fun parseRaw(raw: String): List<String> {
        return raw.split(',', '\n')
            .mapNotNull {
                val trimmed = it.trim()
                if (trimmed.isEmpty()) null else trimmed
            }
    }

    private fun queryDumpMode(ctx: Context): Boolean {
        val cursor = try {
            ctx.contentResolver.query(CONTENT_URI, null, null, null, null)
        } catch (t: Throwable) {
            Log.w(TAG, "queryDumpMode() failed: ${t.message}")
            null
        } ?: return false

        cursor.use { c ->
            if (!c.moveToFirst()) return false
            val keyIdx = c.getColumnIndex("key")
            val valIdx = c.getColumnIndex("value")
            do {
                val key = if (keyIdx >= 0) c.getString(keyIdx) else ""
                val value = if (valIdx >= 0) c.getString(valIdx) else ""
                if (key == KEY_DUMP_MODE) {
                    return value == "1" || value.equals("true", ignoreCase = true)
                }
            } while (c.moveToNext())
        }
        return false
    }

    private fun queryTargets(ctx: Context): List<String> {
        val cursor = try {
            ctx.contentResolver.query(CONTENT_URI, null, null, null, null)
        } catch (t: Throwable) {
            Log.w(TAG, "queryTargets() failed: ${t.message}")
            null
        } ?: return emptyList()

        cursor.use { c ->
            if (!c.moveToFirst()) return emptyList()
            val keyIdx = c.getColumnIndex("key")
            val valIdx = c.getColumnIndex("value")
            val buffer = StringBuilder()
            do {
                val key = if (keyIdx >= 0) c.getString(keyIdx) else ""
                val value = if (valIdx >= 0) c.getString(valIdx) else ""
                if (key == KEY_TARGETS) {
                    buffer.append(value)
                    break
                }
            } while (c.moveToNext())

            val raw = buffer.toString()
            if (raw.isBlank()) {
                Log.i(TAG, "queryTargets(): empty result")
                return emptyList()
            }
            val parsed = parseRaw(raw)
            Log.i(TAG, "queryTargets(): raw=$raw -> ${parsed.size} items")
            return parsed
        }
    }
}

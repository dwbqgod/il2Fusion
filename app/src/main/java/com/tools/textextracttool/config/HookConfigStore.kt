package com.tools.textextracttool.config

import android.content.Context
import android.util.Log
import android.content.ContentValues
import com.tools.textextracttool.config.ConfigContentProvider.Companion.CONTENT_URI
import com.tools.textextracttool.config.ConfigContentProvider.Companion.KEY_RVAS

object HookConfigStore {
    private const val TAG = "[TextExtractTool]"
    fun saveRvas(ctx: Context, rvas: List<Long>) {
        val storeCtx = ctx.applicationContext ?: ctx
        val text = rvas.joinToString(separator = ",")
        val values = ContentValues().apply {
            put("key", KEY_RVAS)
            put("value", text)
        }
        storeCtx.contentResolver.insert(CONTENT_URI, values)
        Log.i(TAG, "saveRvas(): stored ${rvas.size} items -> $text via provider")
    }

    fun loadRvasForApp(ctx: Context): List<Long> {
        val storeCtx = ctx.applicationContext ?: ctx
        return queryRvas(storeCtx)
    }

    fun loadRvasForHook(ctx: Context): List<Long> {
        return queryRvas(ctx)
    }

    fun markHookedPackage(ctx: Context, pkg: String): Set<String> {
        // SharedPreferences 跨进程不可读，这里仅返回当前包用于日志提示
        return setOf(pkg)
    }

    fun enabledPackages(ctx: Context): Set<String> {
        return emptySet()
    }

    private fun parseRaw(raw: String): List<Long> {
        return raw.split(',')
            .mapNotNull {
                val trimmed = it.trim()
                if (trimmed.isEmpty()) null else trimmed.toLongOrNull()
            }
    }

    private fun queryRvas(ctx: Context): List<Long> {
        val cursor = try {
            ctx.contentResolver.query(CONTENT_URI, null, null, null, null)
        } catch (t: Throwable) {
            Log.w(TAG, "queryRvas() failed: ${t.message}")
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
                if (key == KEY_RVAS) {
                    buffer.append(value)
                    break
                }
            } while (c.moveToNext())

            val raw = buffer.toString()
            if (raw.isBlank()) {
                Log.i(TAG, "queryRvas(): empty result")
                return emptyList()
            }
            val parsed = parseRaw(raw)
            Log.i(TAG, "queryRvas(): raw=$raw -> ${parsed.size} items")
            return parsed
        }
    }
}

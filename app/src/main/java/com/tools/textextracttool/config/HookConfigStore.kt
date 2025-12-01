package com.tools.textextracttool.config

import android.content.Context
import android.content.ClipData
import android.content.ClipboardManager
import android.util.Log

object HookConfigStore {
    private const val TAG = "[TextExtractTool]"

    fun saveRvas(ctx: Context, rvas: List<Long>) {
        val text = rvas.joinToString(separator = ",")
        Log.i(TAG, "saveRvas(): stored ${rvas.size} items -> $text")

        // 仅依赖剪贴板做跨进程同步，避免 SharedPreferences 权限问题
        try {
            val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("TextExtractTool RVA", text))
            Log.i(TAG, "saveRvas(): copied to clipboard")
        } catch (t: Throwable) {
            Log.w(TAG, "saveRvas(): clipboard failed ${t.message}")
        }
    }

    fun loadRvas(ctx: Context): List<Long> {
        val raw = readClipboard(ctx).orEmpty()
        if (raw.isBlank()) return emptyList()
        val parsed = parseRaw(raw)
        Log.i(TAG, "loadRvas(): raw=$raw -> ${parsed.size} items")
        return parsed
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

    private fun readClipboard(ctx: Context): String? {
        return try {
            val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            if (!cm.hasPrimaryClip()) return null
            val clip = cm.primaryClip ?: return null
            val item = clip.getItemAt(0)
            val text = item.text?.toString() ?: return null
            Log.i(TAG, "readClipboard(): got ${text.length} chars")
            text
        } catch (t: Throwable) {
            Log.w(TAG, "readClipboard() failed: ${t.message}")
            null
        }
    }
}

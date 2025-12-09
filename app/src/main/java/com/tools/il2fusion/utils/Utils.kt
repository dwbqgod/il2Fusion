package com.tools.il2fusion.utils

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File

object Utils {
    // Common patterns used when parsing Il2Cpp dump outputs.
    val setTextPattern: Regex = Regex("""set_Text\s*\(""", RegexOption.IGNORE_CASE)
    val rvaPattern: Regex = Regex("""RVA:\s*(0x[0-9a-fA-F]+|\d+)""")
    val classDeclPattern: Regex = Regex("""\b(class|struct|interface|enum)\s+([A-Za-z0-9_`]+)""")
    val setterMethodPattern: Regex = Regex("""\b(set_[A-Za-z0-9_]+)\s*\(""", RegexOption.IGNORE_CASE)

    fun resolveDisplayName(context: Context, uri: Uri): String? {
        return try {
            context.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (idx >= 0 && cursor.moveToFirst()) {
                    cursor.getString(idx)
                } else null
            }
        } catch (_: Throwable) {
            null
        } ?: uri.path?.substringAfterLast('/')
    }

    fun buildFileNameFromSource(sourceName: String, newExt: String, defaultBase: String = "dump"): String {
        val base = if (sourceName.contains('.')) {
            sourceName.substringBeforeLast('.')
        } else {
            sourceName
        }.ifBlank { defaultBase }
        val ext = newExt.removePrefix(".").ifBlank { "json" }
        return "$base.$ext"
    }

    /**
     * Moves a temporary file into /sdcard/Download via root shell.
     * Returns the destination path on success, null on failure.
     */
    fun moveToDownload(tempFile: File, destFileName: String): String? {
        val destPath = "/sdcard/Download/$destFileName"
        return try {
            val cmd = arrayOf(
                "su",
                "-c",
                "mkdir -p /sdcard/Download && mv ${tempFile.absolutePath} $destPath"
            )
            val proc = Runtime.getRuntime().exec(cmd)
            val code = proc.waitFor()
            if (code == 0) {
                destPath
            } else {
                tempFile.delete()
                null
            }
        } catch (_: Throwable) {
            tempFile.delete()
            null
        }
    }
}

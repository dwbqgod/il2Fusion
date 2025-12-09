package com.tools.il2fusion.utils

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.File
import org.json.JSONArray
import org.json.JSONObject

/**
 * Handles extracting RVA values from Il2Cpp dumper output files.
 */
class DumpFileParser {

    data class RvaEntry(val functionName: String, val address: String)

    data class RvaParseResult(
        val addresses: List<String>,
        val entries: List<RvaEntry>,
        val savedJsonPath: String?
    )

    /**
     * Reads a C# dump file and returns RVA strings found above set_Text methods.
     */
    suspend fun extractRvas(
        context: Context,
        uri: Uri,
        maxCount: Int
    ): RvaParseResult = withContext(Dispatchers.IO) {
        val sourceName = Utils.resolveDisplayName(context, uri) ?: "dump"
        val resolver = context.contentResolver
        val input = resolver.openInputStream(uri) ?: return@withContext RvaParseResult(
            emptyList(),
            emptyList(),
            null
        )
        val entries = mutableListOf<RvaEntry>()
        input.use { stream ->
            BufferedReader(InputStreamReader(stream)).use { reader ->
                parseDumpLines(reader.readLines(), maxCount, entries)
            }
        }
        val addresses = entries.map { it.address }
        val savedPath = saveJsonToDownload(context, entries, sourceName)
        return@withContext RvaParseResult(addresses, entries, savedPath)
    }

    private fun parseDumpLines(
        lines: List<String>,
        maxCount: Int,
        output: MutableList<RvaEntry>
    ) {
        val setTextPattern = Utils.setTextPattern
        val rvaPattern = Utils.rvaPattern
        val classPattern = Utils.classDeclPattern
        var currentNamespace = ""
        var currentClass = ""

        for (i in 0 until lines.size - 1) {
            val line = lines[i]
            if (line.startsWith("// Namespace:")) {
                currentNamespace = line.substringAfter(":").trim()
            }
            val classMatch = classPattern.find(line)
            if (classMatch != null) {
                currentClass = classMatch.groupValues.getOrNull(2) ?: currentClass
            }

            val next = lines[i + 1]
            if (!setTextPattern.containsMatchIn(next)) continue
            val match = rvaPattern.find(line) ?: continue
            val raw = match.groupValues.getOrNull(1) ?: continue
            val parsed = RvaUtils.parseRva(raw) ?: continue
            val methodName = extractMethodName(next) ?: "set_Text"
            val functionName = buildFunctionName(currentNamespace, currentClass, methodName)
            val address = RvaUtils.formatRva(parsed)
            if (output.any { it.address == address }) continue
            output.add(RvaEntry(functionName = functionName, address = address))
            if (output.size >= maxCount) break
        }
    }

    private fun extractMethodName(line: String): String? {
        return Utils.setterMethodPattern.find(line)?.groupValues?.getOrNull(1)
    }

    private fun buildFunctionName(namespace: String, klass: String, method: String): String {
        val parts = mutableListOf<String>()
        if (namespace.isNotBlank()) parts.add(namespace)
        if (klass.isNotBlank()) parts.add(klass)
        parts.add(method)
        return parts.joinToString(".")
    }

    private fun saveJsonToDownload(context: Context, entries: List<RvaEntry>, sourceName: String): String? {
        if (entries.isEmpty()) return null
        return try {
            val root = JSONObject()
            val array = JSONArray()
            entries.forEach { entry ->
                val obj = JSONObject()
                obj.put("functionName", entry.functionName)
                obj.put("address", entry.address)
                array.put(obj)
            }
            root.put("rvas", array)

            val destFileName = Utils.buildFileNameFromSource(sourceName, "json")
            val tmpFile = File(context.filesDir, "parsed_rvas.json")
            tmpFile.writeText(root.toString(2))

            Utils.moveToDownload(tmpFile, destFileName)
        } catch (t: Throwable) {
            null
        }
    }
}

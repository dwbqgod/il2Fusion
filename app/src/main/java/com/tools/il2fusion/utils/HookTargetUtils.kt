package com.tools.il2fusion.utils

/**
 * Utilities for handling method-based hook inputs.
 */
object HookTargetUtils {
    /**
     * Trims and deduplicates empty entries from user input.
     */
    fun normalizeInputs(inputs: List<String>): List<String> {
        return inputs.mapNotNull {
            val trimmed = it.trim()
            if (trimmed.isEmpty()) null else trimmed
        }
    }

    /**
     * Formats stored targets back to editable strings.
     */
    fun formatInputs(values: List<String>): List<String> {
        return values.map { it.trim() }
    }
}

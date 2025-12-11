package com.tools.il2fusion.ui

import android.content.Context
import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.tools.il2fusion.config.HookConfigRepository
import com.tools.il2fusion.utils.DumpFileParser
import com.tools.il2fusion.utils.HookTargetUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import android.widget.Toast

/**
 * Holds UI state for the hook configuration screen and coordinates data operations.
 */
class HookConfigViewModel(
    private val repository: HookConfigRepository = HookConfigRepository(),
    private val dumpFileParser: DumpFileParser = DumpFileParser()
) : ViewModel() {

    private val _state = MutableStateFlow(HookConfigState())
    val state: StateFlow<HookConfigState> = _state.asStateFlow()

    private val _events = Channel<HookConfigEvent>(Channel.BUFFERED)
    val events = _events.receiveAsFlow()

    /**
     * Loads initial dump mode and method list; empty state shows read-only placeholder.
     */
    fun loadInitial(context: Context) {
        viewModelScope.launch {
            val payload = repository.loadConfig(context)
            if (payload.targets.isEmpty()) {
                _state.value = HookConfigState(
                    methodInputs = emptyList(),
                    savedCount = 0,
                    dumpModeEnabled = payload.dumpModeEnabled
                )
            } else {
                _state.value = HookConfigState(
                    methodInputs = HookTargetUtils.formatInputs(payload.targets),
                    savedCount = payload.targets.size,
                    dumpModeEnabled = payload.dumpModeEnabled
                )
            }
        }
    }

    /**
     * Handles toggling dump mode and pushes the update to storage.
     */
    fun onDumpModeChanged(context: Context, enabled: Boolean) {
        viewModelScope.launch {
            repository.saveDumpMode(context, enabled)
            _state.value = _state.value.copy(dumpModeEnabled = enabled)
            _events.send(HookConfigEvent.ShowMessage(if (enabled) "已切换到 Dump 模式" else "已切换到 文本拦截 模式"))
        }
    }

    /**
     * Triggers a file parse flow to import methods from a dump file.
     */
    fun onFilePicked(context: Context, uri: Uri?) {
        if (uri == null) {
            viewModelScope.launch { _events.send(HookConfigEvent.ShowMessage("未选择文件")) }
            return
        }
        viewModelScope.launch {
            _state.value = _state.value.copy(isLoading = true)
            try {
                val result = dumpFileParser.extractTargets(context, uri, Int.MAX_VALUE)
                val methods = HookTargetUtils.normalizeInputs(result.entries.map { it.functionName })
                if (methods.isNotEmpty()) {
                    repository.saveTargets(context, methods)
                    val formatted = HookTargetUtils.formatInputs(methods)
                    _state.value = _state.value.copy(
                        methodInputs = formatted,
                        savedCount = methods.size
                    )
                    _events.send(HookConfigEvent.ShowMessage("解析并保存 ${methods.size} 个 set_text 方法"))
                } else {
                    _events.send(HookConfigEvent.ShowMessage("未在文件中找到 set_text 方法"))
                }
                if (result.savedJsonPath != null) {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(
                            context.applicationContext,
                            "已保存 JSON 到 ${result.savedJsonPath}",
                            Toast.LENGTH_SHORT
                        ).show()
                    }
                } else if (methods.isNotEmpty()) {
                    _events.send(HookConfigEvent.ShowMessage("JSON 保存失败，已解析方法"))
                }
            } finally {
                _state.value = _state.value.copy(isLoading = false)
            }
        }
    }

    /**
     * Manually persists the current method list to storage (for already解析的数据).
     */
    fun onSave(context: Context) {
        viewModelScope.launch {
            val cleaned = HookTargetUtils.normalizeInputs(_state.value.methodInputs)
            if (cleaned.isEmpty()) {
                _events.send(HookConfigEvent.ShowMessage("请先解析 dump.cs 获取方法列表"))
                return@launch
            }
            repository.saveTargets(context, cleaned)
            val formatted = HookTargetUtils.formatInputs(cleaned)
            _state.value = _state.value.copy(
                methodInputs = formatted,
                savedCount = cleaned.size
            )
            _events.send(HookConfigEvent.ShowMessage("已保存 ${cleaned.size} 个方法"))
        }
    }
}

/**
 * Immutable UI state container for the hook configuration screen.
 */
data class HookConfigState(
    val methodInputs: List<String> = emptyList(),
    val dumpModeEnabled: Boolean = false,
    val savedCount: Int = 0,
    val isLoading: Boolean = false
)

/**
 * UI events dispatched to the screen for user feedback.
 */
sealed class HookConfigEvent {
    data class ShowMessage(val text: String) : HookConfigEvent()
}

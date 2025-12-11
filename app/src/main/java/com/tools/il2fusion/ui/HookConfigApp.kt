package com.tools.il2fusion.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.VerticalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.tools.il2fusion.ui.theme.Il2FusionTheme
import kotlinx.coroutines.flow.collect

@Composable
fun HookConfigApp(viewModel: HookConfigViewModel = viewModel()) {
    Il2FusionTheme {
        val context = LocalContext.current
        val focusManager = LocalFocusManager.current
        val state by viewModel.state.collectAsState()
        val snackbarHostState = remember { SnackbarHostState() }
        val filePickerLauncher = rememberLauncherForActivityResult(
            ActivityResultContracts.OpenDocument()
        ) { uri ->
            viewModel.onFilePicked(context, uri)
        }
        val tabs = remember { SideTab.entries.toList() }
        var selectedTab by rememberSaveable { mutableStateOf(SideTab.Overview) }

        LaunchedEffect(Unit) {
            viewModel.loadInitial(context)
        }

        LaunchedEffect(Unit) {
            viewModel.events.collect { event ->
                when (event) {
                    is HookConfigEvent.ShowMessage -> snackbarHostState.showSnackbar(event.text)
                }
            }
        }

        Scaffold(
            containerColor = Color.Transparent,
            snackbarHost = { SnackbarHost(snackbarHostState) }
        ) { inner ->
            val navWidth = 80.dp
            Row(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(inner)
            ) {
                SideNav(
                    tabs = tabs,
                    selected = selectedTab,
                    onSelect = { selectedTab = it },
                    modifier = Modifier
                        .fillMaxHeight()
                        .width(navWidth)
                )

                VerticalDivider(
                    modifier = Modifier
                        .fillMaxHeight()
                        .width(1.dp),
                    color = MaterialTheme.colorScheme.outlineVariant
                )

                Box(
                    modifier = Modifier
                        .weight(1f)
                        .padding(horizontal = 12.dp)
                        .pointerInput(Unit) {
                            detectTapGestures(onTap = { focusManager.clearFocus() })
                        }
                ) {
                    when (selectedTab) {
                        SideTab.Overview -> HookOverviewScreen(state = state)
                        SideTab.Dump -> DumpModeScreen(
                            state = state,
                            onDumpModeChanged = { viewModel.onDumpModeChanged(context, it) }
                        )
                        SideTab.Parse -> ParseTextScreen(
                            state = state,
                            onPickFile = { filePickerLauncher.launch(arrayOf("*/*")) },
                            onSave = { viewModel.onSave(context) },
                            modifier = Modifier.fillMaxSize()
                        )
                    }
                }
            }
        }
    }
}

enum class SideTab(val title: String) {
    Overview("Home"),
    Dump("Dump"),
    Parse("Parse")
}

@Composable
fun SideNav(
    tabs: List<SideTab>,
    selected: SideTab,
    onSelect: (SideTab) -> Unit,
    modifier: Modifier = Modifier
) {
    androidx.compose.foundation.layout.Column(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.12f))
            .padding(horizontal = 6.dp, vertical = 12.dp),
        verticalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(12.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        tabs.forEach { tab ->
            SideNavItem(
                tab = tab,
                selected = tab == selected,
                onClick = { onSelect(tab) }
            )
        }
    }
}

@Composable
private fun SideNavItem(
    tab: SideTab,
    selected: Boolean,
    onClick: () -> Unit
) {
    val containerColor = if (selected) {
        MaterialTheme.colorScheme.primary.copy(alpha = 0.18f)
    } else {
        MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f)
    }
    val contentColor = if (selected) {
        MaterialTheme.colorScheme.primary
    } else {
        MaterialTheme.colorScheme.onSurface
    }
    Card(
        shape = androidx.compose.foundation.shape.RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = containerColor,
            contentColor = contentColor
        ),
        modifier = Modifier
            .size(52.dp)
            .clickable(onClick = onClick)
    ) {
        androidx.compose.foundation.layout.Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = tab.title,
                style = MaterialTheme.typography.labelLarge.copy(fontWeight = FontWeight.Bold),
                color = contentColor
            )
        }
    }
}

package com.rftester.app.ui.screens.history

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rftester.app.data.local.ReadingEntity
import com.rftester.app.ui.RfViewModel
import com.rftester.app.ui.components.SectionTitle
import com.rftester.app.ui.components.SegmentedSelector
import com.rftester.app.ui.theme.Mint
import com.rftester.app.ui.theme.Sky
import com.rftester.app.ui.theme.Violet
import kotlinx.coroutines.launch

/**
 * تاریخچه — جست‌وجو، فیلتر تاریخ و نود، لیست روان
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HistoryScreen(vm: RfViewModel) {
    val dates by vm.dates.collectAsStateWithLifecycle()
    val n1Name by vm.node1Name.collectAsState()
    val n2Name by vm.node2Name.collectAsState()
    val scope = rememberCoroutineScope()

    var query by remember { mutableStateOf("") }
    var nodeFilter by remember { mutableStateOf(0) }
    var selectedDate by remember { mutableStateOf<String?>(null) }
    var results by remember { mutableStateOf<List<ReadingEntity>>(emptyList()) }
    var dateMenuOpen by remember { mutableStateOf(false) }

    fun reload() {
        scope.launch {
            val nid = when (nodeFilter) {
                1 -> 1
                2 -> 2
                else -> null
            }
            val list = vm.search(query, nid)
            results = if (selectedDate != null) {
                list.filter { it.date_str == selectedDate }
            } else {
                list
            }
        }
    }

    androidx.compose.runtime.LaunchedEffect(query, nodeFilter, selectedDate) {
        reload()
    }

    Scaffold(containerColor = MaterialTheme.colorScheme.background) { pad ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(pad)
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Spacer(Modifier.height(4.dp))

            SectionTitle("تاریخچه", "جست‌وجو در کل دیتابیس محلی (آفلاین هم کار می‌کند)")

            OutlinedTextField(
                value = query,
                onValueChange = { query = it },
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("جست‌وجو: تاریخ، ساعت، دما، رطوبت…") },
                leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
                singleLine = true,
                shape = MaterialTheme.shapes.large
            )

            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                ExposedDropdownMenuBox(
                    expanded = dateMenuOpen,
                    onExpandedChange = { dateMenuOpen = it },
                    modifier = Modifier.weight(1f)
                ) {
                    OutlinedTextField(
                        value = selectedDate ?: "همه تاریخ‌ها",
                        onValueChange = {},
                        readOnly = true,
                        leadingIcon = { Icon(Icons.Default.DateRange, contentDescription = null) },
                        trailingIcon = {
                            ExposedDropdownMenuDefaults.TrailingIcon(dateMenuOpen)
                        },
                        modifier = Modifier
                            .fillMaxWidth()
                            .menuAnchor(),
                        singleLine = true,
                        shape = MaterialTheme.shapes.large
                    )
                    ExposedDropdownMenu(
                        expanded = dateMenuOpen,
                        onDismissRequest = { dateMenuOpen = false }
                    ) {
                        DropdownMenuItem(
                            text = { Text("همه تاریخ‌ها") },
                            onClick = {
                                selectedDate = null
                                dateMenuOpen = false
                            }
                        )
                        dates.forEach { d ->
                            DropdownMenuItem(
                                text = { Text(d) },
                                onClick = {
                                    selectedDate = d
                                    dateMenuOpen = false
                                }
                            )
                        }
                    }
                }
            }

            SegmentedSelector(
                options = listOf("هر دو", n1Name, n2Name),
                selectedIndex = nodeFilter,
                onSelect = { nodeFilter = it }
            )

            Text(
                "${results.size} رکورد",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            LazyColumn(
                verticalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.weight(1f)
            ) {
                items(results, key = { it.id }) { r ->
                    ReadingRow(r)
                }
                item { Spacer(Modifier.height(12.dp)) }
            }
        }
    }
}

@Composable
private fun ReadingRow(r: ReadingEntity) {
    val accent = when (r.node_id) {
        1 -> Sky
        else -> Violet
    }
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        ),
        shape = MaterialTheme.shapes.medium,
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    "N${r.node_id} · سیکل #${r.num_value}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    "${r.date_str}  ${r.time_str}",
                    style = MaterialTheme.typography.titleMedium
                )
            }
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    "%.1f°C".format(r.temp),
                    style = MaterialTheme.typography.titleMedium,
                    color = accent
                )
                Text(
                    "%.1f%%".format(r.humidity),
                    style = MaterialTheme.typography.bodyMedium,
                    color = Mint
                )
            }
            if (r.isPending) {
                Spacer(Modifier.padding(start = 8.dp))
                Text(
                    "⏳",
                    style = MaterialTheme.typography.titleMedium
                )
            }
        }
    }
}

package com.rftester.app.ui.screens.dashboard

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rftester.app.data.local.ReadingEntity
import com.rftester.app.domain.analytics.Analytics
import com.rftester.app.domain.model.Reading
import com.rftester.app.ui.RfViewModel
import com.rftester.app.ui.components.NodeCard
import com.rftester.app.ui.components.SectionTitle
import com.rftester.app.ui.components.StatTile
import com.rftester.app.ui.components.SyncBanner
import com.rftester.app.ui.theme.Amber
import com.rftester.app.ui.theme.Mint
import com.rftester.app.ui.theme.Sky
import com.rftester.app.ui.theme.Violet

/**
 * داشبورد — تمیز و شلوغ نیست:
 * بنر وضعیت + دو کارت نود + نوار آمار سریع + روند ۲۴ ساعته
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DashboardScreen(
    vm: RfViewModel,
    onOpenSettings: () -> Unit
) {
    val sync by vm.syncStatus.collectAsStateWithLifecycle()
    val latest by vm.latest.collectAsStateWithLifecycle()
    val n1Name by vm.node1Name.collectAsState()
    val n2Name by vm.node2Name.collectAsState()
    val total by vm.totalReadings.collectAsState()

    var dayReadings by remember { mutableStateOf<List<ReadingEntity>>(emptyList()) }

    LaunchedEffect(latest.size) {
        dayReadings = vm.loadRange(0, com.rftester.app.ui.TimeRange.D1)
    }

    val node1 = latest.firstOrNull { it.node_id == 1 }
    val node2 = latest.firstOrNull { it.node_id == 2 }

    val spark1 = remember(latest) {
        latest.filter { it.node_id == 1 }.take(30).reversed().map { it.temp }
    }
    val spark2 = remember(latest) {
        latest.filter { it.node_id == 2 }.take(30).reversed().map { it.temp }
    }

    val t1 = Analytics.tempStats(dayReadings.filter { it.node_id == 1 })
    val t2 = Analytics.tempStats(dayReadings.filter { it.node_id == 2 })
    val h1 = Analytics.humidityStats(dayReadings.filter { it.node_id == 1 })
    val h2 = Analytics.humidityStats(dayReadings.filter { it.node_id == 2 })

    val comfort1 = node1?.let { Analytics.comfort(it.temp, it.humidity) }
    val comfort2 = node2?.let { Analytics.comfort(it.temp, it.humidity) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("RF Tester", style = MaterialTheme.typography.titleLarge)
                        Text(
                            "آرشیو دما و رطوبت نود بدنه",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                },
                actions = {
                    IconButton(onClick = { vm.refreshSync() }) {
                        Icon(Icons.Default.Refresh, contentDescription = "همگام‌سازی")
                    }
                    IconButton(onClick = onOpenSettings) {
                        Icon(Icons.Default.Settings, contentDescription = "تنظیمات")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background
                )
            )
        },
        containerColor = MaterialTheme.colorScheme.background
    ) { pad ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(pad)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Spacer(Modifier.height(0.dp))

            SyncBanner(
                online = sync.isOnline,
                pending = sync.pendingCount,
                lastSyncAt = sync.lastSyncAt,
                onSyncClick = { vm.flushNow() }
            )

            if (sync.lastError != null && sync.isOnline) {
                Text(
                    "آخرین خطا: ${sync.lastError}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                NodeCard(
                    nodeTitle = n1Name,
                    reading = node1?.let { vm.toReading(it) },
                    sparkTemps = spark1,
                    accent = Sky,
                    modifier = Modifier.weight(1f)
                )
                NodeCard(
                    nodeTitle = n2Name,
                    reading = node2?.let { vm.toReading(it) },
                    sparkTemps = spark2,
                    accent = Violet,
                    modifier = Modifier.weight(1f)
                )
            }

            SectionTitle("خلاصه ۲۴ ساعت اخیر", "کمینه / بیشینه / میانگین هر نود")

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile(
                    title = "$n1Name · دما",
                    value = if (t1.count > 0) "%.1f°C".format(t1.avg) else "—",
                    subtitle = if (t1.count > 0)
                        "Min ${"%.1f".format(t1.min)} · Max ${"%.1f".format(t1.max)} · n=${t1.count}"
                    else "بدون داده",
                    accent = Sky,
                    modifier = Modifier.weight(1f)
                )
                StatTile(
                    title = "$n2Name · دما",
                    value = if (t2.count > 0) "%.1f°C".format(t2.avg) else "—",
                    subtitle = if (t2.count > 0)
                        "Min ${"%.1f".format(t2.min)} · Max ${"%.1f".format(t2.max)} · n=${t2.count}"
                    else "بدون داده",
                    accent = Violet,
                    modifier = Modifier.weight(1f)
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile(
                    title = "$n1Name · رطوبت",
                    value = if (h1.count > 0) "%.1f%%".format(h1.avg) else "—",
                    subtitle = if (h1.count > 0)
                        "Min ${"%.1f".format(h1.min)} · Max ${"%.1f".format(h1.max)}"
                    else "بدون داده",
                    accent = Mint,
                    modifier = Modifier.weight(1f)
                )
                StatTile(
                    title = "$n2Name · رطوبت",
                    value = if (h2.count > 0) "%.1f%%".format(h2.avg) else "—",
                    subtitle = if (h2.count > 0)
                        "Min ${"%.1f".format(h2.min)} · Max ${"%.1f".format(h2.max)}"
                    else "بدون داده",
                    accent = Amber,
                    modifier = Modifier.weight(1f)
                )
            }

            SectionTitle("وضعیت راحتی")

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile(
                    title = n1Name,
                    value = comfort1?.let { Analytics.comfortLabel(it) } else "—",
                    subtitle = node1?.let { "${it.timeStr} · ${it.dateStr}" } ?: "",
                    accent = Sky,
                    modifier = Modifier.weight(1f)
                )
                StatTile(
                    title = n2Name,
                    value = comfort2?.let { Analytics.comfortLabel(it) } else "—",
                    subtitle = node2?.let { "${it.timeStr} · ${it.dateStr}" } ?: "",
                    accent = Violet,
                    modifier = Modifier.weight(1f)
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile(
                    title = "کل رکوردهای محلی",
                    value = total.toString(),
                    subtitle = "در دیتابیس این دستگاه",
                    modifier = Modifier.weight(1f)
                )
                StatTile(
                    title = "در صف ارسال",
                    value = sync.pendingCount.toString(),
                    subtitle = if (sync.isOnline) "همگام‌سازی فعال" else "منتظر وای‌فای",
                    accent = if (sync.pendingCount > 0) Amber else Mint,
                    modifier = Modifier.weight(1f)
                )
            }

            Spacer(Modifier.height(8.dp))
        }
    }
}

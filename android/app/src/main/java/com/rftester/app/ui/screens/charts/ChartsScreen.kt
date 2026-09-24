package com.rftester.app.ui.screens.charts

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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
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
import com.rftester.app.ui.RfViewModel
import com.rftester.app.ui.TimeRange
import com.rftester.app.ui.charts.ChartMode
import com.rftester.app.ui.charts.CategoryBars
import com.rftester.app.ui.charts.TimeSeriesChart
import com.rftester.app.ui.components.SectionTitle
import com.rftester.app.ui.components.SegmentedSelector
import com.rftester.app.ui.components.SyncBanner
import com.rftester.app.ui.theme.Mint
import com.rftester.app.ui.theme.Sky
import com.rftester.app.ui.theme.Violet

/**
 * صفحه نمودارها — سه حالت گرافیکی (خطی/مساحتی/میله‌ای)
 * + فیلتر نود + متریک دما/رطوبت + بازه زمانی
 */
@Composable
fun ChartsScreen(vm: RfViewModel) {
    val sync by vm.syncStatus.collectAsStateWithLifecycle()
    val n1Name by vm.node1Name.collectAsState()
    val n2Name by vm.node2Name.collectAsState()

    var range by remember { mutableStateOf(TimeRange.D1) }
    var nodeFilter by remember { mutableStateOf(0) } // 0 هر دو، 1، 2
    var metric by remember { mutableStateOf(0) }     // 0 دما، 1 رطوبت
    var mode by remember { mutableStateOf(ChartMode.AREA) }

    var data1 by remember { mutableStateOf<List<ReadingEntity>>(emptyList()) }
    var data2 by remember { mutableStateOf<List<ReadingEntity>>(emptyList()) }

    LaunchedEffect(range, nodeFilter) {
        data1 = if (nodeFilter == 2) emptyList() else vm.loadRange(1, range)
        data2 = if (nodeFilter == 1) emptyList() else vm.loadRange(2, range)
    }

    val isTemp = metric == 0
    val suffix = if (isTemp) "°C" else "%"

    val series1 = remember(data1, metric) {
        if (isTemp) Analytics.series(data1) else Analytics.seriesHumidity(data1)
    }
    val series2 = remember(data2, metric) {
        if (isTemp) Analytics.series(data2) else Analytics.seriesHumidity(data2)
    }

    val hourly = remember(data1, data2, nodeFilter, metric) {
        val base = when (nodeFilter) {
            1 -> data1
            2 -> data2
            else -> data1 + data2
        }
        Analytics.hourlyPattern(base, useHumidity = !isTemp)
            .filter { !it.avg.isNaN() }
            .map { "%02d".format(it.hour) to it.avg }
    }

    val daily = remember(data1, data2, nodeFilter, metric) {
        val base = when (nodeFilter) {
            1 -> data1
            2 -> data2
            else -> data1 + data2
        }
        Analytics.dailyBars(base, useHumidity = !isTemp)
            .takeLast(14)
            .map { it.date.takeLast(5) to it.avg }
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background
    ) { pad ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(pad)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Spacer(Modifier.height(4.dp))

            SyncBanner(
                online = sync.isOnline,
                pending = sync.pendingCount,
                lastSyncAt = sync.lastSyncAt,
                onSyncClick = { vm.flushNow() }
            )

            SectionTitle("نمودارها", "حالت گرافیکی، متریک و بازه را انتخاب کنید")

            SegmentedSelector(
                options = listOf("خطی", "مساحتی", "میله‌ای"),
                selectedIndex = when (mode) {
                    ChartMode.LINE -> 0
                    ChartMode.AREA -> 1
                    ChartMode.BAR -> 2
                },
                onSelect = {
                    mode = when (it) {
                        0 -> ChartMode.LINE
                        1 -> ChartMode.AREA
                        else -> ChartMode.BAR
                    }
                }
            )

            SegmentedSelector(
                options = listOf("دما", "رطوبت"),
                selectedIndex = metric,
                onSelect = { metric = it }
            )

            SegmentedSelector(
                options = listOf("هر دو", n1Name, n2Name),
                selectedIndex = nodeFilter,
                onSelect = { nodeFilter = it }
            )

            SegmentedSelector(
                options = TimeRange.entries.map { it.label },
                selectedIndex = TimeRange.entries.indexOf(range),
                onSelect = { range = TimeRange.entries[it] }
            )

            // سری نود ۱
            if (nodeFilter != 2) {
                Text(
                    "$n1Name — ${if (isTemp) "دما" else "رطوبت"}",
                    style = MaterialTheme.typography.titleMedium
                )
                TimeSeriesChart(
                    points = series1,
                    mode = mode,
                    lineColor = Sky,
                    suffix = suffix,
                    rangeMs = range.ms,
                    modifier = Modifier.fillMaxWidth()
                )
            }

            // سری نود ۲
            if (nodeFilter != 1) {
                Text(
                    "$n2Name — ${if (isTemp) "دما" else "رطوبت"}",
                    style = MaterialTheme.typography.titleMedium
                )
                TimeSeriesChart(
                    points = series2,
                    mode = mode,
                    lineColor = if (nodeFilter == 0) Violet else Sky,
                    suffix = suffix,
                    rangeMs = range.ms,
                    modifier = Modifier.fillMaxWidth()
                )
            }

            // الگوی ساعتی
            Text(
                "الگوی ساعتی (میانگین هر ساعت)",
                style = MaterialTheme.typography.titleMedium
            )
            CategoryBars(
                values = hourly,
                color = if (isTemp) Sky else Mint,
                modifier = Modifier.fillMaxWidth()
            )

            // مقایسه روزانه
            if (daily.size >= 2) {
                Text(
                    "میانگین روزانه (۱۴ روز اخیر)",
                    style = MaterialTheme.typography.titleMedium
                )
                CategoryBars(
                    values = daily,
                    color = Violet,
                    modifier = Modifier.fillMaxWidth()
                )
            }

            Spacer(Modifier.height(10.dp))
        }
    }
}

package com.rftester.app.ui.screens.analytics

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
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rftester.app.data.local.ReadingEntity
import com.rftester.app.domain.analytics.Analytics
import com.rftester.app.ui.RfViewModel
import com.rftester.app.ui.TimeRange
import com.rftester.app.ui.components.SectionTitle
import com.rftester.app.ui.components.SegmentedSelector
import com.rftester.app.ui.components.StatTile
import com.rftester.app.ui.theme.Amber
import com.rftester.app.ui.theme.Mint
import com.rftester.app.ui.theme.Rose
import com.rftester.app.ui.theme.Sky
import com.rftester.app.ui.theme.Violet

/**
 * صفحه تحلیل — آمار دقیق، مقایسه دو نود، همبستگی، ناهنجاری‌ها
 * بدون شلوغی: فقط چند بلوک مشخص
 */
@Composable
fun AnalyticsScreen(vm: RfViewModel) {
    val n1Name by vm.node1Name.collectAsState()
    val n2Name by vm.node2Name.collectAsState()

    var range by remember { mutableStateOf(TimeRange.D7) }
    var data by remember { mutableStateOf<List<ReadingEntity>>(emptyList()) }

    LaunchedEffect(range) {
        data = vm.loadRange(0, range)
    }

    val n1 = remember(data) { data.filter { it.node_id == 1 } }
    val n2 = remember(data) { data.filter { it.node_id == 2 } }

    val t1 = remember(n1) { Analytics.tempStats(n1) }
    val t2 = remember(n2) { Analytics.tempStats(n2) }
    val h1 = remember(n1) { Analytics.humidityStats(n1) }
    val h2 = remember(n2) { Analytics.humidityStats(n2) }
    val comparison = remember(n1, n2) { Analytics.compare(n1, n2) }
    val corr = remember(data) { Analytics.correlation(data) }
    val anomalies = remember(data) { Analytics.anomalies(data) }
    val ch = remember(data) { Analytics.channelSummary(data) }

    Scaffold(containerColor = MaterialTheme.colorScheme.background) { pad ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(pad)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Spacer(Modifier.height(4.dp))

            SectionTitle("تحلیل", "آمار، روند و ناهنجاری‌های بازه انتخابی")

            SegmentedSelector(
                options = TimeRange.entries.map { it.label },
                selectedIndex = TimeRange.entries.indexOf(range),
                onSelect = { range = TimeRange.entries[it] }
            )

            if (data.isEmpty()) {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surface
                    ),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text(
                        "در این بازه داده‌ای وجود ندارد.\nداده‌ها آفلاین ذخیره و بعد از اتصال ارسال/دریافت می‌شوند.",
                        modifier = Modifier.padding(20.dp),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                return@Column
            }

            // ---- آمار دما ----
            SectionTitle("دما", "$n1Name مقابل $n2Name")
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile("میانگین N1", "%.1f°C".format(t1.avg),
                    "σ = %.2f".format(t1.stdDev), Sky, Modifier.weight(1f))
                StatTile("میانگین N2", "%.1f°C".format(t2.avg),
                    "σ = %.2f".format(t2.stdDev), Violet, Modifier.weight(1f))
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile("کمینه N1", "%.1f°C".format(t1.min),
                    "دامنه %.1f".format(t1.range), Sky, Modifier.weight(1f))
                StatTile("بیشینه N1", "%.1f°C".format(t1.max),
                    "میانه %.1f".format(t1.median), Sky, Modifier.weight(1f))
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile("کمینه N2", "%.1f°C".format(t2.min),
                    "دامنه %.1f".format(t2.range), Violet, Modifier.weight(1f))
                StatTile("بیشینه N2", "%.1f°C".format(t2.max),
                    "میانه %.1f".format(t2.median), Violet, Modifier.weight(1f))
            }

            // ---- آمار رطوبت ----
            SectionTitle("رطوبت")
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                StatTile("میانگین N1", "%.1f%%".format(h1.avg),
                    "Min ${"%.1f".format(h1.min)} · Max ${"%.1f".format(h1.max)}",
                    Mint, Modifier.weight(1f))
                StatTile("میانگین N2", "%.1f%%".format(h2.avg),
                    "Min ${"%.1f".format(h2.min)} · Max ${"%.1f".format(h2.max)}",
                    Amber, Modifier.weight(1f))
            }

            // ---- مقایسه دو نود ----
            SectionTitle("مقایسه دو نود بدنه")
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            ) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text(
                        "اختلاف میانگین دما: %.2f °C".format(comparison.avgGap),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    if (comparison.avgGap < 1.5f) {
                        Text(
                            "✓ هر دو نود هم‌دما هستند — کالیبراسیون مناسب است.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = Mint
                        )
                    } else {
                        Text(
                            "⚠ اختلاف محسوس — بررسی محل نصب یا کالیبراسیون توصیه می‌شود.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = Amber
                        )
                    }

                    Text(
                        "روند N1: %s%.1f °C در این بازه".format(
                            if (t1.trend >= 0) "↑ " else "↓ ",
                            kotlin.math.abs(t1.trend)
                        ),
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (t1.trend >= 0) Rose else Sky
                    )
                    Text(
                        "روند N2: %s%.1f °C در این بازه".format(
                            if (t2.trend >= 0) "↑ " else "↓ ",
                            kotlin.math.abs(t2.trend)
                        ),
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (t2.trend >= 0) Rose else Sky
                    )
                }
            }

            // ---- همبستگی دما/رطوبت ----
            SectionTitle("همبستگی دما ↔ رطوبت", "ضریب پیرسون بین -۱ تا +۱")
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surface
                )
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text(
                        "%.2f".format(corr),
                        style = MaterialTheme.typography.displayMedium,
                        color = when {
                            corr < -0.3f -> Sky
                            corr > 0.3f -> Amber
                            else -> Mint
                        }
                    )
                    Spacer(Modifier.height(8.dp))
                    // نوار -1..1
                    LinearProgressIndicator(
                        progress = { ((corr + 1f) / 2f).coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth(),
                        color = when {
                            corr < -0.3f -> Sky
                            corr > 0.3f -> Amber
                            else -> Mint
                        },
                        trackColor = MaterialTheme.colorScheme.surfaceVariant
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        when {
                            corr < -0.5f -> "همبستگی منفی قوی: با بالا رفتن دما، رطوبت می‌کاهد (الگوی معمول)."
                            corr < -0.15f -> "همبستگی منفی ملایم میان دما و رطوبت."
                            corr > 0.5f -> "همبستگی مثبت قوی: دما و رطوبت هم‌جهت حرکت می‌کنند — بررسی کنید."
                            corr > 0.15f -> "همبستگی مثبت ملایم."
                            else -> "ارتباط معناداری دیده نمی‌شود."
                        },
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            // ---- ناهنجاری‌ها ----
            SectionTitle("ناهنجاری‌های دمایی", "بیش از ۲.۵σ از میانگین")
            if (anomalies.isEmpty()) {
                StatTile(
                    "وضعیت", "✓ بدون ناهنجاری",
                    "${data.size} نمونه بررسی شد",
                    Mint,
                    Modifier.fillMaxWidth()
                )
            } else {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = Rose.copy(alpha = 0.10f)
                    ),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(
                            "${anomalies.size} مورد ناهنجار",
                            style = MaterialTheme.typography.titleMedium,
                            color = Rose,
                            fontWeight = FontWeight.Bold
                        )
                        anomalies.takeLast(8).reversed().forEach { a ->
                            Text(
                                "• ${a.dateStr} ${a.timeStr} — N${a.node_id} — %.1f°C".format(a.temp),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurface
                            )
                        }
                    }
                }
            }

            // ---- کانال‌ها (فقط دو NBCM واقعی) ----
            SectionTitle("وضعیت دو کانال NBCM", "از ${ch.total} رکورد")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (i in 0..1) {
                    val pct = if (ch.total == 0) 0f
                    else ch.ch[i] * 100f / ch.total
                    StatTile(
                        title = "NBCM${i + 1}",
                        value = "%.0f%%".format(pct),
                        subtitle = "${ch.ch[i]} فعال",
                        accent = if (pct > 50f) Mint else Amber,
                        modifier = Modifier.weight(1f)
                    )
                }
            }

            Spacer(Modifier.height(12.dp))
        }
    }
}

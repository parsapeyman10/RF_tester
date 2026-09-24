package com.rftester.app.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AssistChipDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SuggestionChip
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.rftester.app.domain.model.Reading
import com.rftester.app.ui.charts.Sparkline
import com.rftester.app.ui.theme.Amber
import com.rftester.app.ui.theme.Mint
import com.rftester.app.ui.theme.Rose
import com.rftester.app.ui.theme.Sky
import com.rftester.app.util.TimeFmt

/** نوار وضعیت اتصال / صف ارسال — همیشه شفاف برای کاربر */
@Composable
fun SyncBanner(
    online: Boolean,
    pending: Int,
    lastSyncAt: Long?,
    onSyncClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val bg = when {
        !online && pending > 0 -> Amber.copy(alpha = 0.15f)
        online && pending > 0 -> Sky.copy(alpha = 0.15f)
        online -> Mint.copy(alpha = 0.12f)
        else -> Rose.copy(alpha = 0.12f)
    }
    val dot = when {
        !online && pending > 0 -> Amber
        online && pending > 0 -> Sky
        online -> Mint
        else -> Rose
    }
    val text = when {
        !online && pending > 0 ->
            "آفلاین — $pending مورد در صف ارسال (ذخیره شد، بعداً ارسال می‌شود)"
        !online -> "آفلاین — ذخیره محلی فعال است"
        pending > 0 -> "$pending مورد در حال ارسال…"
        lastSyncAt != null -> "آنلاین — همگام‌شده ${TimeFmt.clock(lastSyncAt)}"
        else -> "آنلاین — آماده دریافت/ارسال"
    }

    Card(
        onClick = onSyncClick,
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = bg),
        shape = RoundedCornerShape(16.dp)
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(10.dp)
                    .background(dot, CircleShape)
            )
            Spacer(Modifier.width(10.dp))
            Text(
                text,
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.weight(1f)
            )
            Text(
                if (online) "بروزرسانی" else "تلاش مجدد",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold
            )
        }
    }
}

/** کارت بزرگ نمایش آخرین وضعیت یک نود */
@Composable
fun NodeCard(
    nodeTitle: String,
    reading: Reading?,
    sparkTemps: List<Float>,
    accent: Color,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface
        ),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(8.dp)
                        .background(accent, CircleShape)
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    nodeTitle,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.weight(1f)
                )
                if (reading != null) {
                    Text(
                        reading.timeStr,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            Spacer(Modifier.height(10.dp))

            if (reading == null) {
                Text(
                    "هنوز داده‌ای ثبت نشده",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            } else {
                Row(verticalAlignment = Alignment.Bottom) {
                    Text(
                        "%.1f".format(reading.temp),
                        style = MaterialTheme.typography.displayLarge,
                        color = accent
                    )
                    Text(
                        " °C",
                        style = MaterialTheme.typography.titleLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(bottom = 8.dp)
                    )
                    Spacer(Modifier.weight(1f))
                    Column(horizontalAlignment = Alignment.End) {
                        Text(
                            "رطوبت",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                        Text(
                            "%.1f %%".format(reading.humidity),
                            style = MaterialTheme.typography.headlineSmall,
                            color = Mint
                        )
                    }
                }

                Spacer(Modifier.height(8.dp))
                Sparkline(values = sparkTemps, color = accent)
                Spacer(Modifier.height(6.dp))

                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    StatusChip("N1", reading.n1)
                    StatusChip("N2", reading.n2)
                    StatusChip("N3", reading.n3)
                    StatusChip("N4", reading.n4)
                    if (reading.syncPending) {
                        SuggestionChip(
                            onClick = {},
                            label = { Text("در صف") },
                            colors = AssistChipDefaults.suggestionChipColors(
                                containerColor = Amber.copy(alpha = 0.18f)
                            )
                        )
                    }
                }
                Spacer(Modifier.height(4.dp))
                Text(
                    "سیکل #${reading.numValue} · ${reading.dateStr}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

@Composable
fun StatusChip(label: String, active: Boolean) {
    AssistChip(
        onClick = {},
        label = {
            Text(
                label,
                color = if (active) Color(0xFF052E16) else MaterialTheme.colorScheme.onSurfaceVariant
            )
        },
        colors = AssistChipDefaults.assistChipColors(
            containerColor = if (active) Mint else MaterialTheme.colorScheme.surfaceVariant
        )
    )
}

/** کارت آمار کوچک */
@Composable
fun StatTile(
    title: String,
    value: String,
    subtitle: String = "",
    accent: Color = MaterialTheme.colorScheme.primary,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier,
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)
    ) {
        Column(Modifier.padding(12.dp)) {
            Text(
                title,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(4.dp))
            Text(
                value,
                style = MaterialTheme.typography.titleLarge,
                color = accent,
                fontWeight = FontWeight.Bold
            )
            if (subtitle.isNotBlank()) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

/** سگمنت انتخاب بازه/متریک/حالت نمودار */
@Composable
fun SegmentedSelector(
    options: List<String>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(14.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.6f)
        )
    ) {
        Row(
            modifier = Modifier.padding(4.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            options.forEachIndexed { i, label ->
                val selected = i == selectedIndex
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .clickable { onSelect(i) }
                        .background(
                            color = if (selected) MaterialTheme.colorScheme.primary
                            else Color.Transparent,
                            shape = RoundedCornerShape(10.dp)
                        )
                        .padding(vertical = 8.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        label,
                        style = MaterialTheme.typography.labelLarge,
                        color = if (selected) MaterialTheme.colorScheme.onPrimary
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                        fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium
                    )
                }
            }
        }
    }
}

@Composable
fun SectionTitle(title: String, subtitle: String = "") {
    Column {
        Text(title, style = MaterialTheme.typography.titleLarge)
        if (subtitle.isNotBlank()) {
            Text(
                subtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

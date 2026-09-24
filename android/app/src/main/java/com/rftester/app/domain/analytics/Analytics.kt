package com.rftester.app.domain.analytics

import com.rftester.app.data.local.ReadingEntity
import com.rftester.app.domain.model.ChartPoint
import com.rftester.app.domain.model.NodeComparison
import com.rftester.app.domain.model.Stats
import kotlin.math.abs
import kotlin.math.sqrt

/**
 * موتور تحلیل — سبک، بدون وابستگی، روی همان داده‌های Room.
 * شامل: آمار، روند، الگوی ساعتی، همبستگی دما/رطوبت، شاخص راحتی.
 */
object Analytics {

    // ------------------------------------------------------------------
    // آمار پایه
    // ------------------------------------------------------------------
    fun stats(values: List<Float>): Stats {
        if (values.isEmpty()) {
            return Stats(0, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f)
        }
        val sorted = values.sorted()
        val n = sorted.size
        val min = sorted.first()
        val max = sorted.last()
        val avg = values.average().toFloat()
        val median = if (n % 2 == 1) {
            sorted[n / 2]
        } else {
            (sorted[n / 2 - 1] + sorted[n / 2]) / 2f
        }
        val variance = values.map { (it - avg) * (it - avg) }.average().toFloat()
        return Stats(
            count = n,
            min = min,
            max = max,
            avg = avg,
            median = median,
            first = values.first(),
            last = values.last(),
            range = max - min,
            stdDev = sqrt(variance)
        )
    }

    fun tempStats(readings: List<ReadingEntity>): Stats =
        stats(readings.map { it.temp })

    fun humidityStats(readings: List<ReadingEntity>): Stats =
        stats(readings.map { it.humidity })

    // ------------------------------------------------------------------
    // سری زمانی برای نمودار — نمونه‌برداری حداکثری برای روانی رسم
    // ------------------------------------------------------------------
    fun series(readings: List<ReadingEntity>, maxPoints: Int = 240): List<ChartPoint> {
        if (readings.isEmpty()) return emptyList()
        if (readings.size <= maxPoints) {
            return readings.map { ChartPoint(it.ts, it.temp) }
        }
        val step = readings.size / maxPoints
        val out = ArrayList<ChartPoint>(maxPoints + 1)
        var i = 0
        while (i < readings.size) {
            val r = readings[i]
            out.add(ChartPoint(r.ts, r.temp))
            i += step
        }
        // همیشه جدیدترین نقطه را نگه دار
        val last = readings.last()
        if (out.lastOrNull()?.ts != last.ts) {
            out.add(ChartPoint(last.ts, last.temp))
        }
        return out
    }

    fun seriesHumidity(readings: List<ReadingEntity>, maxPoints: Int = 240): List<ChartPoint> {
        if (readings.isEmpty()) return emptyList()
        if (readings.size <= maxPoints) {
            return readings.map { ChartPoint(it.ts, it.humidity) }
        }
        val step = readings.size / maxPoints
        val out = ArrayList<ChartPoint>(maxPoints + 1)
        var i = 0
        while (i < readings.size) {
            val r = readings[i]
            out.add(ChartPoint(r.ts, r.humidity))
            i += step
        }
        val last = readings.last()
        if (out.lastOrNull()?.ts != last.ts) {
            out.add(ChartPoint(last.ts, last.humidity))
        }
        return out
    }

    // ------------------------------------------------------------------
    // براکت ساعتی (الگوی شبانه‌روزی)
    // ------------------------------------------------------------------
    data class HourBucket(val hour: Int, val avg: Float, val count: Int)

    fun hourlyPattern(readings: List<ReadingEntity>, useHumidity: Boolean = false): List<HourBucket> {
        val buckets = Array(24) { floatArrayOf(0f, 0f) } // sum, count
        val zone = java.time.ZoneId.of("Asia/Tehran")
        for (r in readings) {
            val h = java.time.LocalDateTime.ofInstant(
                java.time.Instant.ofEpochMilli(r.ts),
                zone
            ).hour
            val v = if (useHumidity) r.humidity else r.temp
            buckets[h][0] += v
            buckets[h][1] += 1f
        }
        return buckets.mapIndexed { h, arr ->
            val sum = arr[0]
            val cnt = arr[1]
            if (cnt > 0) HourBucket(h, sum / cnt, cnt.toInt()) else HourBucket(h, Float.NaN, 0)
        }
    }

    // ------------------------------------------------------------------
    // شاخص راحتی انسانی (ساده‌شده از THI)
    // ------------------------------------------------------------------
    enum class Comfort { COLD, COOL, COMFORTABLE, WARM, HOT, HUMID }

    fun comfort(temp: Float, humidity: Float): Comfort = when {
        temp < 10f -> Comfort.COLD
        temp in 10f..16f -> Comfort.COOL
        temp in 16f..26f && humidity in 30f..65f -> Comfort.COMFORTABLE
        temp in 16f..26f && humidity > 65f -> Comfort.HUMID
        temp in 26f..32f -> Comfort.WARM
        else -> Comfort.HOT
    }

    fun comfortLabel(c: Comfort): String = when (c) {
        Comfort.COLD -> "سرد"
        Comfort.COOL -> "خنک"
        Comfort.COMFORTABLE -> "مطبوع"
        Comfort.WARM -> "گرم"
        Comfort.HOT -> "داغ"
        Comfort.HUMID -> "مرطوب"
    }

    // ------------------------------------------------------------------
    // همبستگی پیرسون دما ↔ رطوبت
    // ------------------------------------------------------------------
    fun correlation(readings: List<ReadingEntity>): Float {
        val n = readings.size
        if (n < 3) return 0f
        val xs = readings.map { it.temp }
        val ys = readings.map { it.humidity }
        val mx = xs.average().toFloat()
        val my = ys.average().toFloat()
        var num = 0f
        var dx = 0f
        var dy = 0f
        for (i in 0 until n) {
            val a = xs[i] - mx
            val b = ys[i] - my
            num += a * b
            dx += a * a
            dy += b * b
        }
        val den = sqrt(dx) * sqrt(dy)
        return if (den == 0f) 0f else (num / den).coerceIn(-1f, 1f)
    }

    // ------------------------------------------------------------------
    // مقایسه دو نود
    // ------------------------------------------------------------------
    fun compare(n1: List<ReadingEntity>, n2: List<ReadingEntity>): NodeComparison {
        val s1 = tempStats(n1)
        val s2 = tempStats(n2)
        val gap = if (s1.count > 0 && s2.count > 0) abs(s1.avg - s2.avg) else 0f
        // همبستگی دما↔رطوبت روی ترکیب هر دو نود (در صفحه تحلیل جدا محاسبه می‌شود)
        val combined = (n1 + n2).sortedBy { it.ts }
        val corr = correlation(combined)
        return NodeComparison(s1, s2, gap, corr)
    }

    // ------------------------------------------------------------------
    // تشخیص ناهنجاری (بیش از ۲.۵ انحراف معیار از میانگین)
    // ------------------------------------------------------------------
    fun anomalies(readings: List<ReadingEntity>, thresholdZ: Float = 2.5f): List<ReadingEntity> {
        if (readings.size < 10) return emptyList()
        val s = tempStats(readings)
        if (s.stdDev == 0f) return emptyList()
        return readings.filter { abs(it.temp - s.avg) / s.stdDev > thresholdZ }
    }

    // ------------------------------------------------------------------
    // تعداد خاموش/روشنبودن کانال‌ها (NBCM)
    // ------------------------------------------------------------------
    data class ChannelSummary(
        val total: Int,
        val ch: List<Int> // تعداد فعال برای NBCM1..4
    )

    fun channelSummary(readings: List<ReadingEntity>): ChannelSummary {
        val total = readings.size
        val counts = intArrayOf(0, 0, 0, 0)
        for (r in readings) {
            for (b in 0..3) {
                if (r.nbcm_mask and (1 shl b) != 0) counts[b]++
            }
        }
        return ChannelSummary(total, counts.toList())
    }

    // ------------------------------------------------------------------
    // خوشه‌بندی روزانه برای نمودار میله‌ای روزها
    // ------------------------------------------------------------------
    data class DayBar(val date: String, val avg: Float, val min: Float, val max: Float, val count: Int)

    fun dailyBars(readings: List<ReadingEntity>, useHumidity: Boolean = false): List<DayBar> {
        return readings.groupBy { it.date_str }
            .toSortedMap()
            .map { (date, list) ->
                val vals = if (useHumidity) list.map { it.humidity } else list.map { it.temp }
                DayBar(date, vals.average().toFloat(), vals.min(), vals.max(), list.size)
            }
    }
}

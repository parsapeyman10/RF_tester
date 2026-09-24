package com.rftester.app.util

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

object TimeFmt {
    private val tehran: ZoneId = ZoneId.of("Asia/Tehran")

    private val hm = DateTimeFormatter.ofPattern("HH:mm")
    private val hms = DateTimeFormatter.ofPattern("HH:mm:ss")
    private val dayMon = DateTimeFormatter.ofPattern("d MMM")
    private val fullFa = DateTimeFormatter.ofPattern("yyyy/MM/dd HH:mm")

    fun clock(ts: Long): String =
        Instant.ofEpochMilli(ts).atZone(tehran).format(hms)

    fun hourMinute(ts: Long): String =
        Instant.ofEpochMilli(ts).atZone(tehran).format(hm)

    fun dayLabel(ts: Long): String =
        Instant.ofEpochMilli(ts).atZone(tehran).format(dayMon)

    fun full(ts: Long): String =
        Instant.ofEpochMilli(ts).atZone(tehran).format(fullFa)

    fun nowDate(): String =
        java.time.LocalDate.now(tehran).toString()

    /** برچسب محور X بسته به بازه انتخابی */
    fun axisLabel(ts: Long, rangeMs: Long): String =
        when {
            rangeMs <= 6L * 3600_000L -> hourMinute(ts)
            rangeMs <= 48L * 3600_000L -> hourMinute(ts)
            else -> dayLabel(ts)
        }
}

object Vars {
    const val RANGES = 6
}

package com.rftester.app.domain.model

/**
 * مدل خوانا/نوشتنی یک نمونهٔ سنسور.
 * زمان به‌صورت epoch-millis ذخیره می‌شود (سبک، صحیح، بدون ابهام فرمت)
 * و افزون بر آن date/time رشته‌ای برای نمایش و پارتیشن روزانه نگه داشته می‌شود.
 */
data class Reading(
    val id: String,
    val nodeId: Int,            // 1 = نود بدنه ۱ ، 2 = نود بدنه ۲
    val numValue: Int,          // شماره سیکل/کارد دستگاه
    val temp: Float,            // دما (°C) — مقدار صحیح REAL
    val humidity: Float,        // رطوبت (%) — مقدار صحیح REAL
    val ts: Long,               // epoch millis (زمان لحظه ثبت)
    val dateStr: String,        // yyyy-MM-dd
    val timeStr: String,        // HH:mm:ss
    val nbcmMask: Int,          // بیت‌فیلد NBCM1..4 (bit0..bit3)
    val syncPending: Boolean,   // true = هنوز به سرور نرسیده (Store & Forward)
    val source: Int             // 0=سرور 1=دستی 2=ESP
) {
    val n1: Boolean get() = nbcmMask and 0b0001 != 0
    val n2: Boolean get() = nbcmMask and 0b0010 != 0
    val n3: Boolean get() = nbcmMask and 0b0100 != 0
    val n4: Boolean get() = nbcmMask and 0b1000 != 0

    companion object {
        const val SOURCE_SERVER = 0
        const val SOURCE_LOCAL = 1
        const val SOURCE_ESP = 2

        fun mask(n1: Boolean, n2: Boolean, n3: Boolean, n4: Boolean): Int =
            (if (n1) 1 else 0) or
            (if (n2) 2 else 0) or
            (if (n3) 4 else 0) or
            (if (n4) 8 else 0)
    }
}

data class NodeInfo(
    val id: Int,
    val name: String,
    val subtitle: String
)

/** نقطهٔ سبک برای رسم نمودار */
data class ChartPoint(
    val ts: Long,
    val value: Float
)

/** آمار تحلیلی یک بازه */
data class Stats(
    val count: Int,
    val min: Float,
    val max: Float,
    val avg: Float,
    val median: Float,
    val first: Float,
    val last: Float,
    val range: Float,
    val stdDev: Float
) {
    val trend: Float get() = last - first
}

/** نتیجهٔ تحلیل ترکیبی دو نود */
data class NodeComparison(
    val node1: Stats,
    val node2: Stats,
    val avgGap: Float,
    val correlation: Float // -1..1
)

/** وضعیت اتصال و همگام‌سازی */
data class SyncStatus(
    val isOnline: Boolean,
    val pendingCount: Int,
    val lastSyncAt: Long?,
    val lastError: String?
)

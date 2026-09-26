package ir.electronicperspective.rftester

/**
 * پروتکل حالت دیباگ/هات‌اسپات ESP32 (TaskInternalWiFiConnection -> MODE_HOTSPOT_VIEW).
 *
 * کلاینت یک خط متنی می‌فرستد:
 *   "sync"    -> آخرین رکورد ذخیره‌شده روی SD
 *   "sync10"  -> حداکثر ۱۰ رکورد آخر، داخل یک آرایه: "[" ... "]" با کاما بین اعضا
 *
 * هر رکورد یک آبجکت JSON در یک خط است:
 *   {"ID":12,"T":23.45,"H":51.20,"N1":1,"N2":0,"Time":"2026-01-05 13:04:09"}
 *
 * اگر دیتایی نباشد پاسخ دقیقاً "NO_DATA" است.
 *
 * این کلاس عمداً هیچ وابستگی‌ای به اندروید ندارد تا در unit test قابل تست باشد.
 */
object EspProtocol {

    const val CMD_SYNC_LAST = "sync"
    const val CMD_SYNC_10 = "sync10"
    const val NO_DATA = "NO_DATA"

    data class Reading(
        val id: Int,
        val temp: Double,
        val humidity: Double,
        val nbcm1: Boolean,
        val nbcm2: Boolean,
        val timestamp: String
    ) {
        fun pretty(): String = buildString {
            append("#").append(id).append("  ").append(timestamp).append('\n')
            append("   T = ").append(String.format("%.2f", temp)).append(" C")
            append("   |   H = ").append(String.format("%.2f", humidity)).append(" %")
            append('\n')
            append("   NBCM1 = ").append(if (nbcm1) "OK" else "NOK")
            append("   |   NBCM2 = ").append(if (nbcm2) "OK" else "NOK")
        }
    }

    /** آیا پاسخ یعنی «هیچ رکوردی روی SD نیست»؟ */
    fun isNoData(raw: String): Boolean = raw.contains(NO_DATA)

    /**
     * استخراج همه‌ی رکوردها از پاسخ خام.
     * پاسخ ممکن است چندخطی باشد، کاما و براکت آرایه داشته باشد یا حتی ناقص
     * برسد؛ بنابراین به‌جای پارس کل رشته، هر آبجکت {...} جدا پارس می‌شود.
     */
    fun parseRecords(raw: String): List<Reading> {
        val objects = Regex("\\{[^{}]*}").findAll(raw).map { it.value }
        val result = ArrayList<Reading>()
        for (chunk in objects) {
            val reading = parseSingle(chunk) ?: continue
            result.add(reading)
        }
        return result
    }

    // عمداً از org.json استفاده نمی‌کنیم: در unit test های JVM موجود نیست
    // و پیام «not mocked» می‌دهد. فرمت هم ثابت و ساده است.
    private fun field(chunk: String, key: String): String? =
        Regex("\"$key\"\\s*:\\s*\"?([^,\"}]*)\"?").find(chunk)?.groupValues?.get(1)?.trim()

    private fun parseSingle(chunk: String): Reading? {
        val id = field(chunk, "ID")?.toIntOrNull() ?: return null
        if (id < 0) return null
        return Reading(
            id = id,
            temp = field(chunk, "T")?.toDoubleOrNull() ?: Double.NaN,
            humidity = field(chunk, "H")?.toDoubleOrNull() ?: Double.NaN,
            nbcm1 = (field(chunk, "N1")?.toIntOrNull() ?: 0) != 0,
            nbcm2 = (field(chunk, "N2")?.toIntOrNull() ?: 0) != 0,
            timestamp = field(chunk, "Time").orEmpty()
        )
    }
}

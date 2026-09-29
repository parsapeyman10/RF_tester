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
    const val CMD_SYNC_ALL = "syncall"
    const val CMD_INFO = "info"
    const val NO_DATA = "NO_DATA"
    const val END_MARK = "END"

    /** فرمت واحد پروژه — عیناً همان چیزی که app.py هم پارس می‌کند */
    private val LINE_RE = Regex(
        "NUM=(-?\\d+)," +
            "(?:BCM1_OPEN|NBCM1)=([A-Za-z0-9]+),(?:BCM1_CLOSE|NBCM2)=([A-Za-z0-9]+)," +
            "(?:BCM2_OPEN|NBCM3)=([A-Za-z0-9]+),(?:BCM2_CLOSE|NBCM4)=([A-Za-z0-9]+)," +
            "Temp=(-?\\d+(?:\\.\\d+)?),Humidity=(-?\\d+(?:\\.\\d+)?)," +
            "Date=(\\d{4})-(\\d{1,2})-(\\d{1,2})," +
            "Time=(\\d{1,2}):(\\d{1,2}):(\\d{1,2})"
    )

    private fun truthy(v: String) = v.uppercase() in setOf("OK", "1", "TRUE", "YES")

    data class Reading(
        val id: Int,
        val temp: Double,
        val humidity: Double,
        val nbcm1: Boolean,
        val nbcm2: Boolean,
        val timestamp: String,
        val nbcm3: Boolean = false,
        val nbcm4: Boolean = false,
        /** خط خام NUM=... که بدون تغییر به app.py فرستاده می‌شود */
        val rawLine: String = ""
    ) {
        fun pretty(): String = buildString {
            append("#").append(id).append("  ").append(timestamp).append('\n')
            append("   T = ").append(String.format("%.2f", temp)).append(" C")
            append("   |   H = ").append(String.format("%.2f", humidity)).append(" %")
            append('\n')
            append("   BCM1 باز = ").append(if (nbcm1) "OK" else "NOK")
            append("   |   BCM1 بسته = ").append(if (nbcm2) "OK" else "NOK")
            append("\n   BCM2 باز = ").append(if (nbcm3) "OK" else "NOK")
            append("   |   BCM2 بسته = ").append(if (nbcm4) "OK" else "NOK")
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
        val result = ArrayList<Reading>()

        // 1) فرمت واحد NUM=...  (اولویت)
        for (line in raw.lineSequence()) {
            val t = line.trim()
            if (t.isEmpty() || t == END_MARK || t == NO_DATA) continue
            val m = LINE_RE.find(t) ?: continue
            val g = m.groupValues
            result.add(
                Reading(
                    id = g[1].toIntOrNull() ?: continue,
                    nbcm1 = truthy(g[2]),
                    nbcm2 = truthy(g[3]),
                    nbcm3 = truthy(g[4]),
                    nbcm4 = truthy(g[5]),
                    temp = g[6].toDoubleOrNull() ?: Double.NaN,
                    humidity = g[7].toDoubleOrNull() ?: Double.NaN,
                    timestamp = String.format(
                        "%04d-%02d-%02d %02d:%02d:%02d",
                        g[8].toInt(), g[9].toInt(), g[10].toInt(),
                        g[11].toInt(), g[12].toInt(), g[13].toInt()
                    ),
                    rawLine = m.value
                )
            )
        }
        if (result.isNotEmpty()) return result

        // 2) سازگاری عقب‌رو با فریمور قدیمی (JSON)
        val objects = Regex("\\{[^{}]*}").findAll(raw).map { it.value }
        for (chunk in objects) {
            val reading = parseSingle(chunk) ?: continue
            result.add(reading)
        }
        return result
    }

    /** خط‌های آماده برای POST به /api/ingest سرور Flask */
    fun toServerLines(records: List<Reading>): List<String> = records.map { r ->
        if (r.rawLine.isNotEmpty()) r.rawLine else String.format(
            "NUM=%d,BCM1_OPEN=%s,BCM1_CLOSE=%s,BCM2_OPEN=%s,BCM2_CLOSE=%s," +
                "Temp=%.2f,Humidity=%.2f,Date=%s,Time=%s",
            r.id,
            if (r.nbcm1) "OK" else "NOK", if (r.nbcm2) "OK" else "NOK",
            if (r.nbcm3) "OK" else "NOK", if (r.nbcm4) "OK" else "NOK",
            r.temp, r.humidity,
            r.timestamp.substringBefore(" ").ifEmpty { "1970-01-01" },
            r.timestamp.substringAfter(" ", "00:00:00")
        )
    }

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

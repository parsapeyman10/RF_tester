package ir.electronicperspective.rftester

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * این تست‌ها دقیقاً همان رشته‌ای را می‌دهند که فریمور ESP32 در
 * sendDataFile() با snprintf تولید می‌کند. اگر فرمت سمت فریمور عوض شود،
 * این تست در CI قرمز می‌شود.
 */
class EspProtocolTest {

    private val single =
        "{\"ID\":42,\"T\":23.45,\"H\":51.20,\"N1\":1,\"N2\":0,\"Time\":\"2026-01-05 13:04:09\"}"

    @Test
    fun parsesSingleRecord() {
        val records = EspProtocol.parseRecords(single)
        assertEquals(1, records.size)
        val r = records[0]
        assertEquals(42, r.id)
        assertEquals(23.45, r.temp, 0.001)
        assertEquals(51.20, r.humidity, 0.001)
        assertTrue(r.nbcm1)
        assertFalse(r.nbcm2)
        assertEquals("2026-01-05 13:04:09", r.timestamp)
    }

    @Test
    fun parsesSync10Array() {
        val raw = buildString {
            append("[\n")
            append(single).append(",\n")
            append(single.replace("\"ID\":42", "\"ID\":43")).append("\n")
            append("]\n")
        }
        val records = EspProtocol.parseRecords(raw)
        assertEquals(2, records.size)
        assertEquals(listOf(42, 43), records.map { it.id })
    }

    @Test
    fun detectsNoData() {
        assertTrue(EspProtocol.isNoData("NO_DATA"))
        assertFalse(EspProtocol.isNoData(single))
        assertTrue(EspProtocol.parseRecords("NO_DATA").isEmpty())
    }

    @Test
    fun ignoresGarbage() {
        assertTrue(EspProtocol.parseRecords("[HOTSPOT] User Connected.").isEmpty())
    }
}

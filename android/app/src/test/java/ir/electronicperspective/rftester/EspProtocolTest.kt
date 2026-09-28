package ir.electronicperspective.rftester

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * این تست‌ها دقیقاً همان خطی را می‌دهند که فریمور ESP32 در
 * formatRecordLine() تولید می‌کند — همان فرمتی که app.py هم می‌خواند.
 * اگر فرمت سمت فریمور عوض شود، این تست در CI قرمز می‌شود.
 */
class EspProtocolTest {

    private val line1 = "NUM=42,NBCM1=OK,NBCM2=NOK,NBCM3=OK,NBCM4=NOK," +
        "Temp=23.45,Humidity=51.20,Date=2026-01-05,Time=13:04:09"
    private val line2 = "NUM=43,NBCM1=OK,NBCM2=OK,NBCM3=NOK,NBCM4=NOK," +
        "Temp=-4.50,Humidity=88.00,Date=2026-01-05,Time=13:06:09"

    @Test
    fun parsesUnifiedLine() {
        val records = EspProtocol.parseRecords("$line1\nEND\n")
        assertEquals(1, records.size)
        val r = records[0]
        assertEquals(42, r.id)
        assertEquals(23.45, r.temp, 0.001)
        assertEquals(51.20, r.humidity, 0.001)
        assertTrue(r.nbcm1)
        assertFalse(r.nbcm2)
        assertTrue(r.nbcm3)
        assertEquals("2026-01-05 13:04:09", r.timestamp)
        assertEquals(line1, r.rawLine)
    }

    @Test
    fun parsesMultipleLines() {
        val raw = "$line1\n$line2\nEND\n"
        val records = EspProtocol.parseRecords(raw)
        assertEquals(2, records.size)
        assertEquals(listOf(42, 43), records.map { it.id })
        assertEquals(-4.50, records[1].temp, 0.001)
    }

    @Test
    fun roundTripToServerLines() {
        val records = EspProtocol.parseRecords("$line1\n$line2\nEND")
        val lines = EspProtocol.toServerLines(records)
        // خطی که به app.py می‌رود باید عیناً همان خط دستگاه باشد
        assertEquals(listOf(line1, line2), lines)
    }

    @Test
    fun stillParsesLegacyJson() {
        val json = "{\"ID\":7,\"T\":20.00,\"H\":30.00,\"N1\":1,\"N2\":0,\"Time\":\"2026-01-05 10:00:00\"}"
        val records = EspProtocol.parseRecords(json)
        assertEquals(1, records.size)
        assertEquals(7, records[0].id)
    }

    @Test
    fun detectsNoDataAndGarbage() {
        assertTrue(EspProtocol.isNoData("NO_DATA"))
        assertTrue(EspProtocol.parseRecords("NO_DATA\nEND").isEmpty())
        assertTrue(EspProtocol.parseRecords("[VIEW] Client connected.").isEmpty())
        assertTrue(EspProtocol.parseRecords("NUM=44,NBCM1=OK,Temp=20.0").isEmpty())
    }
}

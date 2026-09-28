package ir.electronicperspective.rftester

import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

/**
 * ارسال رکوردها به سرور Flask (app.py) — مسیر /api/ingest
 *
 * همان کاری که کلاینت دسکتاپ می‌کند؛ یعنی چه گوشی وصل شود چه کامپیوتر،
 * دیتا با یک فرمت و از یک مسیر وارد دیتابیس می‌شود.
 */
object ServerClient {

    data class Result(val saved: Int, val duplicates: Int, val invalid: Int)

    @Throws(Exception::class)
    fun push(serverUrl: String, lines: List<String>): Result {
        val url = URL(serverUrl.trimEnd('/') + "/api/ingest")
        val conn = (url.openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = 5000
            readTimeout = 10000
            doOutput = true
            setRequestProperty("Content-Type", "application/json")
            setRequestProperty("X-Device", "android-app")
        }

        val body = JSONObject().put("lines", JSONArray(lines)).toString()
        conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }

        val stream = if (conn.responseCode in 200..299) conn.inputStream else conn.errorStream
        val text = BufferedReader(InputStreamReader(stream, Charsets.UTF_8)).use { it.readText() }
        conn.disconnect()

        val json = JSONObject(text)
        return Result(
            saved = json.optInt("saved", 0),
            duplicates = json.optInt("duplicates", 0),
            invalid = json.optInt("invalid", 0)
        )
    }
}

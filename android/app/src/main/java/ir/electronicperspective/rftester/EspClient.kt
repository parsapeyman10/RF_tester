package ir.electronicperspective.rftester

import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * کلاینت TCP ساده برای ESP32 در حالت هات‌اسپات.
 *
 * نکته‌ی مهم درباره‌ی فریمور فعلی: سمت ESP32 بعد از پاسخ، سوکت را نمی‌بندد و
 * منتظر دستور بعدی می‌ماند. پس نمی‌توان تا EOF خواند؛ با SO_TIMEOUT کوتاه
 * می‌خوانیم و وقتی مکث پیش آمد یا آرایه بسته شد، خواندن را تمام می‌کنیم.
 */
class EspClient(
    private val host: String,
    private val port: Int,
    private val connectTimeoutMs: Int = 4000,
    private val readTimeoutMs: Int = 1500,
    private val overallTimeoutMs: Long = 8000
) {

    /** دستور را می‌فرستد و پاسخ خام را برمی‌گرداند. */
    @Throws(Exception::class)
    fun send(command: String): String {
        Socket().use { socket ->
            socket.tcpNoDelay = true
            socket.connect(InetSocketAddress(host, port), connectTimeoutMs)
            socket.soTimeout = readTimeoutMs

            val writer = OutputStreamWriter(socket.getOutputStream(), Charsets.US_ASCII)
            // فریمور با readStringUntil('\n') می‌خواند؛ پس '\n' الزامی است.
            writer.write(command)
            writer.write("\n")
            writer.flush()

            val reader = BufferedReader(InputStreamReader(socket.getInputStream(), Charsets.US_ASCII))
            val sb = StringBuilder()
            val deadline = System.currentTimeMillis() + overallTimeoutMs

            while (System.currentTimeMillis() < deadline) {
                val line = try {
                    reader.readLine()
                } catch (e: SocketTimeoutException) {
                    // مکث در ارسال = پایان پاسخ (اگر چیزی گرفته‌ایم)
                    if (sb.isNotEmpty()) break else continue
                } ?: break

                sb.append(line).append('\n')

                if (EspProtocol.isNoData(line)) break
                // پایان آرایه‌ی sync10
                if (line.trim() == "]") break
                // پاسخ تک‌رکوردی sync
                if (command == EspProtocol.CMD_SYNC_LAST && line.contains('}')) break
            }
            return sb.toString().trim()
        }
    }
}

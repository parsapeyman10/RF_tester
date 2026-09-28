package ir.electronicperspective.rftester

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import ir.electronicperspective.rftester.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private var lastRecords: List<EspProtocol.Reading> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.syncButton.setOnClickListener { run(EspProtocol.CMD_SYNC_LAST) }
        binding.sync10Button.setOnClickListener { run(EspProtocol.CMD_SYNC_10) }
        binding.syncAllButton.setOnClickListener { run(EspProtocol.CMD_SYNC_ALL) }
        binding.pushButton.setOnClickListener { pushToServer() }
        binding.clearButton.setOnClickListener {
            binding.outputText.text = ""
            lastRecords = emptyList()
        }
    }

    private fun run(command: String) {
        val host = binding.hostInput.text.toString().trim().ifEmpty { "192.168.1.1" }
        val port = binding.portInput.text.toString().trim().toIntOrNull() ?: 80

        setBusy(true)
        binding.statusText.text = getString(R.string.status_connecting)

        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { EspClient(host, port).send(command) }
            }
            setBusy(false)

            result.onSuccess { raw ->
                when {
                    raw.isEmpty() -> {
                        binding.statusText.text = "پاسخی از دستگاه نیامد (تایم‌اوت)"
                    }

                    EspProtocol.isNoData(raw) -> {
                        binding.statusText.text = "دستگاه رکوردی روی SD ندارد"
                    }

                    else -> {
                        val records = EspProtocol.parseRecords(raw)
                        lastRecords = records
                        binding.statusText.text = "دریافت شد: ${records.size} رکورد"
                        binding.outputText.text = if (records.isEmpty()) {
                            raw
                        } else {
                            records.joinToString("\n\n") { it.pretty() }
                        }
                    }
                }
            }.onFailure { e ->
                binding.statusText.text = "خطا: ${e.message ?: e.javaClass.simpleName}"
            }
        }
    }

    /** همان دیتا، همان فرمت، همان مقصد که کلاینت دسکتاپ استفاده می‌کند */
    private fun pushToServer() {
        if (lastRecords.isEmpty()) {
            binding.statusText.text = "اول دیتا بگیرید"
            return
        }
        val server = binding.serverInput.text.toString().trim()
        if (server.isEmpty()) {
            binding.statusText.text = "آدرس سرور app.py را وارد کنید"
            return
        }

        setBusy(true)
        binding.statusText.text = "در حال ارسال به app.py …"
        val lines = EspProtocol.toServerLines(lastRecords)

        lifecycleScope.launch {
            val res = withContext(Dispatchers.IO) {
                runCatching { ServerClient.push(server, lines) }
            }
            setBusy(false)
            res.onSuccess {
                binding.statusText.text =
                    "app.py: ${it.saved} ذخیره، ${it.duplicates} تکراری، ${it.invalid} نامعتبر"
            }.onFailure { e ->
                binding.statusText.text = "خطای ارسال: ${e.message ?: e.javaClass.simpleName}"
            }
        }
    }

    private fun setBusy(busy: Boolean) {
        binding.syncButton.isEnabled = !busy
        binding.sync10Button.isEnabled = !busy
        binding.syncAllButton.isEnabled = !busy
        binding.pushButton.isEnabled = !busy
    }
}

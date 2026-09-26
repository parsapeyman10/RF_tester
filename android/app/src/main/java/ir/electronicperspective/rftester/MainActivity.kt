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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.syncButton.setOnClickListener { run(EspProtocol.CMD_SYNC_LAST) }
        binding.sync10Button.setOnClickListener { run(EspProtocol.CMD_SYNC_10) }
        binding.clearButton.setOnClickListener { binding.outputText.text = "" }
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

    private fun setBusy(busy: Boolean) {
        binding.syncButton.isEnabled = !busy
        binding.sync10Button.isEnabled = !busy
    }
}

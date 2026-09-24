package com.rftester.app.ui.screens.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.CloudUpload
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rftester.app.ui.RfViewModel
import com.rftester.app.ui.components.SectionTitle

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    vm: RfViewModel,
    onBack: () -> Unit
) {
    val server by vm.serverUrl.collectAsState()
    val n1 by vm.node1Name.collectAsState()
    val n2 by vm.node2Name.collectAsState()
    val dark by vm.darkMode.collectAsState()
    val retain by vm.retainDays.collectAsState()
    val sync by vm.syncStatus.collectAsStateWithLifecycle()
    val total by vm.totalReadings.collectAsState()

    var serverField by remember(server) { mutableStateOf(server) }
    var n1Field by remember(n1) { mutableStateOf(n1) }
    var n2Field by remember(n2) { mutableStateOf(n2) }
    var retainField by remember(retain) { mutableStateOf(retain.toString()) }
    var showClear by remember { mutableStateOf(false) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("تنظیمات") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "بازگشت")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background
                )
            )
        },
        containerColor = MaterialTheme.colorScheme.background
    ) { pad ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(pad)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            SectionTitle("سرور", "آدرس Flask برای ارسال/دریافت داده")

            OutlinedTextField(
                value = serverField,
                onValueChange = { serverField = it },
                modifier = Modifier.fillMaxWidth(),
                label = { Text("http://192.168.x.x:5000") },
                singleLine = true,
                shape = MaterialTheme.shapes.large
            )

            Button(
                onClick = { vm.saveServerUrl(serverField) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Icon(Icons.Default.CloudUpload, contentDescription = null)
                Spacer(Modifier.padding(4.dp))
                Text("ذخیره آدرس و همگام‌سازی")
            }

            if (sync.lastError != null) {
                Text(
                    "خطا: ${sync.lastError}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error
                )
            }

            HorizontalDivider()

            SectionTitle("نودهای محصول", "نام دو محصول نود بدنه")

            OutlinedTextField(
                value = n1Field,
                onValueChange = { n1Field = it },
                modifier = Modifier.fillMaxWidth(),
                label = { Text("نام نود ۱") },
                singleLine = true,
                shape = MaterialTheme.shapes.large
            )
            OutlinedTextField(
                value = n2Field,
                onValueChange = { n2Field = it },
                modifier = Modifier.fillMaxWidth(),
                label = { Text("نام نود ۲") },
                singleLine = true,
                shape = MaterialTheme.shapes.large
            )
            OutlinedButton(
                onClick = { vm.saveNodeNames(n1Field, n2Field) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("ذخیره نام‌ها")
            }

            HorizontalDivider()

            SectionTitle("دیتابیس", "سبک، کامل: زمان، تاریخ، دما، رطوبت")

            Card(
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surface
                ),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        "رکوردهای محلی: $total",
                        style = MaterialTheme.typography.titleMedium
                    )
                    Text(
                        "در صف ارسال: ${sync.pendingCount}",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        "بدون وای‌فای ذخیره می‌شود و به‌محض اتصال، ارسال خواهد شد.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Text(
                        "آخرین همگام‌سازی: " + (sync.lastSyncAt?.let {
                            com.rftester.app.util.TimeFmt.full(it)
                        } ?: "—"),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            OutlinedTextField(
                value = retainField,
                onValueChange = { retainField = it.filter { c -> c.isDigit() } },
                modifier = Modifier.fillMaxWidth(),
                label = { Text("مدت نگهداری (روز)") },
                singleLine = true,
                shape = MaterialTheme.shapes.large
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = {
                        retainField.toIntOrNull()?.let { vm.saveRetainDays(it) }
                    },
                    modifier = Modifier.weight(1f)
                ) { Text("اعمال مدت نگهداری") }
                OutlinedButton(
                    onClick = { vm.pruneNow() },
                    modifier = Modifier.weight(1f)
                ) { Text("پاک‌سازی الان") }
            }

            Card(
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.error.copy(alpha = 0.08f)
                ),
                modifier = Modifier.fillMaxWidth()
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text(
                        "حذف کل داده‌های محلی",
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.error
                    )
                    Spacer(Modifier.height(6.dp))
                    OutlinedButton(
                        onClick = { showClear = true },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Icon(Icons.Default.Delete, contentDescription = null)
                        Spacer(Modifier.padding(4.dp))
                        Text("پاک‌کردن دیتابیس اپ")
                    }
                }
            }

            HorizontalDivider()

            SectionTitle("ظاهر")
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("حالت تیره", modifier = Modifier.weight(1f))
                Switch(checked = dark, onCheckedChange = { vm.saveDarkMode(it) })
            }

            Spacer(Modifier.height(24.dp))
        }
    }

    if (showClear) {
        AlertDialog(
            onDismissRequest = { showClear = false },
            title = { Text("حذف کامل؟") },
            text = {
                Text("همه رکوردهای محلی پاک می‌شوند. این عملیات برگشت‌پذیر نیست.")
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        vm.clearAllData()
                        showClear = false
                    }
                ) {
                    Text("حذف", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { showClear = false }) {
                    Text("انصراف")
                }
            }
        )
    }
}

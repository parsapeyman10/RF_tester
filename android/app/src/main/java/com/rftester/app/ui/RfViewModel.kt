package com.rftester.app.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.rftester.app.RfApp
import com.rftester.app.data.local.ReadingEntity
import com.rftester.app.domain.analytics.Analytics
import com.rftester.app.domain.model.SyncStatus
import com.rftester.app.util.TimeFmt
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import java.time.LocalDate
import java.time.ZoneId

enum class TimeRange(val label: String, val ms: Long) {
    H1("۱ ساعت", 3600_000L),
    H6("۶ ساعت", 6 * 3600_000L),
    D1("۲۴ ساعت", 24 * 3600_000L),
    D7("۷ روز", 7 * 24 * 3600_000L),
    D30("۳۰ روز", 30L * 24 * 3600_000L),
    ALL("همه", Long.MAX_VALUE);

    fun window(now: Long = System.currentTimeMillis()): Pair<Long, Long> =
        if (this == ALL) 0L to now else (now - ms) to now
}

/** ViewModel مشترک همه صفحات */
class RfViewModel(private val app: RfApp) : ViewModel() {

    private val repo get() = app.container.readingRepository
    private val settings get() = app.container.settings

    val isOnline: StateFlow<Boolean> = repo.isOnline
    val pendingCount = repo.pendingCount.stateIn(viewModelScope, SharingStarted.Eagerly, 0)
    val lastSyncAt: StateFlow<Long?> = repo.lastSyncAt
    val lastError: StateFlow<String?> = repo.lastError
    val latest: StateFlow<List<ReadingEntity>> =
        repo.latest.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())
    val dates: StateFlow<List<String>> =
        repo.dates.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    val serverUrl = MutableStateFlow(settings.serverUrl)
    val node1Name = MutableStateFlow(settings.node1Name)
    val node2Name = MutableStateFlow(settings.node2Name)
    val darkMode = MutableStateFlow(settings.darkMode)
    val retainDays = MutableStateFlow(settings.retainDays)

    // انتخاب مشترک صفحات نمودار/تحلیل
    val selectedRange = MutableStateFlow(TimeRange.D1)
    val selectedNode = MutableStateFlow(0) // 0=هر دو، 1، 2
    val selectedMetric = MutableStateFlow(0) // 0=دما، 1=رطوبت

    val syncStatus: StateFlow<SyncStatus> = combine(
        isOnline, pendingCount, lastSyncAt, lastError
    ) { online, pending, syncAt, err ->
        SyncStatus(online, pending, syncAt, err)
    }.stateIn(viewModelScope, SharingStarted.Eagerly, SyncStatus(false, 0, null, null))

    val totalReadings = MutableStateFlow(0L)

    init {
        viewModelScope.launch {
            totalReadings.value = repo.count()
        }
        refreshSync()
    }

    // ------------------------------------------------------------------
    // اقدامات
    // ------------------------------------------------------------------

    fun refreshSync() {
        viewModelScope.launch {
            repo.syncNow()
            totalReadings.value = repo.count()
        }
    }

    fun flushNow() {
        viewModelScope.launch {
            repo.syncNow()
        }
    }

    /** ثبت دستی سریع (نمونه آزمایشی/اورژانسی) */
    fun recordQuick(nodeId: Int, temp: Float, humidity: Float) {
        viewModelScope.launch {
            repo.record(nodeId, temp, humidity, source = 1)
            totalReadings.value = repo.count()
        }
    }

    fun saveServerUrl(url: String) {
        settings.serverUrl = url
        serverUrl.value = settings.serverUrl
        refreshSync()
    }

    fun saveNodeNames(n1: String, n2: String) {
        settings.node1Name = n1.ifBlank { "نود بدنه ۱" }
        settings.node2Name = n2.ifBlank { "نود بدنه ۲" }
        node1Name.value = settings.node1Name
        node2Name.value = settings.node2Name
        viewModelScope.launch { repo.renameNodes(settings.node1Name, settings.node2Name) }
    }

    fun saveDarkMode(v: Boolean) {
        settings.darkMode = v
        darkMode.value = v
    }

    fun saveRetainDays(days: Int) {
        settings.retainDays = days.coerceIn(7, 3650)
        retainDays.value = settings.retainDays
        viewModelScope.launch { repo.prune(settings.retainDays) }
    }

    fun pruneNow() {
        viewModelScope.launch { repo.prune(retainDays.value) }
    }

    fun clearAllData() {
        viewModelScope.launch {
            repo.clearAll()
            totalReadings.value = 0
        }
    }

    // ------------------------------------------------------------------
    // داده خام برای صفحات
    // ------------------------------------------------------------------

    suspend fun loadRange(nodeId: Int, range: TimeRange): List<ReadingEntity> {
        val (from, to) = range.window()
        return if (nodeId == 0) {
            repo.rangeAll(from, to)
        } else {
            repo.range(nodeId, from, to)
        }
    }

    suspend fun search(q: String, nodeId: Int?): List<ReadingEntity> =
        repo.search(nodeId, q)

    /** آخرین خوانش هر نود از لیست latest */
    fun latestFor(nodeId: Int): ReadingEntity? =
        latest.value.firstOrNull { it.node_id == nodeId }

    fun toReading(e: ReadingEntity): Reading = repo.readEntity(e)

    fun todayIso(): String = TimeFmt.nowDate()

    companion object {
        val Factory = object : ViewModelProvider.Factory {
            @Suppress("UNCHECKED_CAST")
            override fun <T : ViewModel> create(modelClass: Class<T>): T {
                return RfViewModel(RfApp.instance) as T
            }
        }
    }
}

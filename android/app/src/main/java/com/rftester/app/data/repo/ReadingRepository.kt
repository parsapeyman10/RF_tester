package com.rftester.app.data.repo

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import com.rftester.app.data.local.NodeEntity
import com.rftester.app.data.local.RfDatabase
import com.rftester.app.data.local.ReadingEntity
import com.rftester.app.data.local.SyncMetaEntity
import com.rftester.app.data.remote.IngestItem
import com.rftester.app.data.remote.IngestRequest
import com.rftester.app.data.remote.ServerReading
import com.rftester.app.domain.model.Reading
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.UUID

/**
 * مخزن اصلی — قلب دیتابیس اپ.
 *
 * اصل «آفلاین‌فرست»:
 *  - هر خوانش اول در Room (SQLite) ثبت می‌شود؛ بدون وای‌فای هم کار می‌کند.
 *  - اگر آنلاین باشد، بلافاصله flush به سرور می‌شود.
 *  - اگر آفلاین باشد، sync_state=1 می‌ماند و WorkManager بعد از اتصال ارسال می‌کند.
 *  - Pull دوطرفه: داده سرور هم با کلید یکتا merge می‌شود (بدون تکرار).
 */
class ReadingRepository(
    private val context: Context,
    private val db: RfDatabase,
    private val settings: SettingsRepository
) {
    private val dao = db.readingDao()
    private val syncDao = db.syncDao()

    private val _isOnline = MutableStateFlow(false)
    val isOnline: StateFlow<Boolean> = _isOnline.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    private val _lastSyncAt = MutableStateFlow<Long?>(null)
    val lastSyncAt: StateFlow<Long?> = _lastSyncAt.asStateFlow()

    val pendingCount: Flow<Int> = dao.pendingCountFlow()
    val latest: Flow<List<ReadingEntity>> = dao.latestFlow(200)
    val nodes: Flow<List<NodeEntity>> = dao.nodesFlow()
    val dates: Flow<List<String>> = dao.distinctDatesFlow()

    init {
        registerConnectivityWatcher()
    }

    // ---------------------------------------------------------------------
    // اتصال
    // ---------------------------------------------------------------------

    private fun registerConnectivityWatcher() {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val request = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        cm.registerNetworkCallback(request, object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                _isOnline.value = true
            }
            override fun onLost(network: Network) {
                _isOnline.value = cm.allNetworks.any {
                    cm.getNetworkCapabilities(it)
                        ?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true
                }
            }
        })
        _isOnline.value = isCurrentlyOnline(cm)
    }

    private fun isCurrentlyOnline(cm: ConnectivityManager = context.getSystemService(
        Context.CONNECTIVITY_SERVICE) as ConnectivityManager): Boolean {
        val caps = cm.getNetworkCapabilities(cm.activeNetwork) ?: return false
        return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    }

    suspend fun isOnlineNow(): Boolean = withContext(Dispatchers.IO) {
        isCurrentlyOnline()
    }

    // ---------------------------------------------------------------------
    // نوشتن (ثبت خوانش جدید)
    // ---------------------------------------------------------------------

    suspend fun record(
        nodeId: Int,
        temp: Float,
        humidity: Float,
        numValue: Int = 0,
        nbcmMask: Int = 0,
        ts: Long = System.currentTimeMillis(),
        source: Int = Reading.SOURCE_LOCAL,
        id: String? = null
    ): String = withContext(Dispatchers.IO) {
        val ldt = LocalDateTime.ofInstant(Instant.ofEpochMilli(ts), tehranZone())
        val date = ldt.format(DateTimeFormatter.ofPattern("yyyy-MM-dd"))
        val time = ldt.format(DateTimeFormatter.ofPattern("HH:mm:ss"))
        val rid = id ?: UUID.randomUUID().toString()
        val entity = ReadingEntity(
            id = rid,
            node_id = nodeId,
            num_value = numValue,
            temp = temp,
            humidity = humidity,
            ts = ts,
            date_str = date,
            time_str = time,
            nbcm_mask = nbcmMask,
            sync_state = 1, // همیشه اول در صف
            source = source,
            dedupe_key = dedupeKey(nodeId, numValue, ts, temp, humidity)
        )
        dao.insert(entity)

        // اگر آنلاینیم، همین حالا تلاش به ارسال (بهترین UX)
        if (_isOnline.value) {
            runCatching { flushPending() }
        }
        rid
    }

    // ---------------------------------------------------------------------
    // Store & Forward — ارسال صف به سرور
    // ---------------------------------------------------------------------

    suspend fun flushPending(batchSize: Int = 200): Int = withContext(Dispatchers.IO) {
        var sentTotal = 0
        try {
            val api = settings.api()
            while (true) {
                val batch = dao.pendingBatch(batchSize)
                if (batch.isEmpty()) break

                val items = batch.map { e ->
                    IngestItem(
                        id = e.id,
                        nodeId = e.node_id,
                        numValue = e.num_value,
                        temp = e.temp,
                        humidity = e.humidity,
                        ts = e.ts,
                        date = e.date_str,
                        time = e.time_str,
                        nbcmMask = e.nbcm_mask,
                        source = e.source
                    )
                }

                val resp = api.ingest(IngestRequest(readings = items))
                if (resp.isSuccessful) {
                    dao.markSynced(batch.map { it.id })
                    sentTotal += batch.size
                    _lastError.value = null
                    touchMeta(success = true)
                } else {
                    _lastError.value = "HTTP ${resp.code()}"
                    touchMeta(success = false)
                    break
                }
                if (batch.size < batchSize) break
            }
        } catch (t: Throwable) {
            _lastError.value = t.message ?: t.javaClass.simpleName
            touchMeta(success = false)
        }
        sentTotal
    }

    // ---------------------------------------------------------------------
    // Pull — دریافت داده سرور و ادغام بدون تکرار
    // ---------------------------------------------------------------------

    suspend fun pullFromServer(limit: Int = 5000): Int = withContext(Dispatchers.IO) {
        var merged = 0
        try {
            val api = settings.api()
            val resp = api.masterData(limit = limit)
            if (!resp.isSuccessful || resp.body() == null) {
                _lastError.value = "HTTP ${resp.code()}"
                return@withContext 0
            }
            val list = resp.body().orEmpty()
            val entities = list.mapNotNull { toEntity(it) }
            if (entities.isNotEmpty()) {
                val ids = dao.insertAll(entities)
                merged = ids.count { it != -1L }
            }
            _lastError.value = null
            touchMeta(success = true)
        } catch (t: Throwable) {
            _lastError.value = t.message ?: t.javaClass.simpleName
            touchMeta(success = false)
        }
        merged
    }

    /**
     * یک چرخه کامل همگام‌سازی:
     * 1) ابتدا ارسال داده‌های ذخیره‌شده آفلاین (مهم‌تر)
     * 2) سپس دریافت جدیدترین‌های سرور
     */
    suspend fun syncNow(): Pair<Int, Int> {
        val pushed = if (isOnlineNow()) flushPending() else 0
        val pulled = if (isOnlineNow()) pullFromServer() else 0
        return pushed to pulled
    }

    /** تبدیل رکورد سرور → Entity با سازگاری فرمت زمان */
    private fun toEntity(r: ServerReading): ReadingEntity? {
        return try {
            val nodeId = (r.node_id ?: 1).coerceIn(1, 2)
            val temp = r.temp?.toFloatOrNull() ?: return null
            val humidity = r.humidity?.toFloatOrNull() ?: return null
            val (ts, date, time) = resolveTime(r)
            val mask = maskFromStatuses(r.nbcm_statuses)
            val num = r.num_value ?: 0
            ReadingEntity(
                id = "srv-$nodeId-$num-$ts",
                node_id = nodeId,
                num_value = num,
                temp = temp,
                humidity = humidity,
                ts = ts,
                date_str = date,
                time_str = time,
                nbcm_mask = mask,
                sync_state = 0, // از سرور آمده؛ نباید دوباره ارسال شود
                source = Reading.SOURCE_SERVER,
                dedupe_key = dedupeKey(nodeId, num, ts, temp, humidity)
            )
        } catch (_: Throwable) {
            null
        }
    }

    private fun resolveTime(r: ServerReading): Triple<Long, String, String> {
        // اولویت: date + time صریح سرور
        if (!r.date.isNullOrBlank() && !r.time.isNullOrBlank()) {
            val d = r.date.trim()
            val t = r.time.trim().substringBefore(".").take(8)
            val normalized = "$d $t"
            runCatching {
                val ldt = LocalDateTime.parse(
                    normalized,
                    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
                )
                return Triple(ldt.atZone(tehranZone()).toInstant().toEpochMilli(), d, t)
            }
        }
        // فرمت timestamp سرور: 2026-01-01T12:30:45  یا با میلی‌ثانیه/تایم‌زون
        val raw = r.timestamp
        if (!raw.isNullOrBlank()) {
            runCatching {
                val cleaned = raw.replace('T', ' ').substringBefore("+").substringBefore(".")
                    .trim()
                val ldt = LocalDateTime.parse(
                    cleaned,
                    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
                )
                return Triple(
                    ldt.atZone(tehranZone()).toInstant().toEpochMilli(),
                    ldt.format(DateTimeFormatter.ofPattern("yyyy-MM-dd")),
                    ldt.format(DateTimeFormatter.ofPattern("HH:mm:ss"))
                )
            }
        }
        // fallback: حال
        val now = System.currentTimeMillis()
        val ldt = LocalDateTime.ofInstant(Instant.ofEpochMilli(now), tehranZone())
        return Triple(
            now,
            ldt.format(DateTimeFormatter.ofPattern("yyyy-MM-dd")),
            ldt.format(DateTimeFormatter.ofPattern("HH:mm:ss"))
        )
    }

    private fun maskFromStatuses(map: Map<String, String>?): Int {
        if (map == null) return 0
        var mask = 0
        for (i in 1..4) {
            val v = map["NBCM$i"] ?: continue
            if (v.equals("active", true) || v.equals("OK", true)) {
                mask = mask or (1 shl (i - 1))
            }
        }
        return mask
    }

    private suspend fun touchMeta(success: Boolean) {
        runCatching {
            val old = syncDao.meta() ?: SyncMetaEntity()
            syncDao.upsertMeta(
                old.copy(
                    last_sync_at = if (success) System.currentTimeMillis() else old.last_sync_at,
                    last_error = if (success) null else _lastError.value,
                    last_server_ip = settings.serverUrl
                )
            )
            if (success) _lastSyncAt.value = System.currentTimeMillis()
        }
    }

    // ---------------------------------------------------------------------
    // کوئری‌های نمایشی / تحلیلی
    // ---------------------------------------------------------------------

    suspend fun range(nodeId: Int, from: Long, to: Long): List<ReadingEntity> =
        dao.range(nodeId, from, to)

    suspend fun rangeAll(from: Long, to: Long): List<ReadingEntity> =
        dao.rangeAll(from, to)

    suspend fun search(nodeId: Int?, q: String, limit: Int = 300): List<ReadingEntity> =
        dao.search(nodeId, q, limit)

    suspend fun distinctDates(): List<String> = dao.distinctDates()

    suspend fun count(): Long = dao.count()

    suspend fun prune(keepDays: Int): Int {
        val cutoff = System.currentTimeMillis() - keepDays * 86_400_000L
        return dao.pruneOlderThan(cutoff)
    }

    suspend fun clearAll(): Int = dao.clearAll()

    suspend fun renameNodes(n1: String, n2: String) {
        settings.node1Name = n1
        settings.node2Name = n2
        dao.upsertNode(NodeEntity(1, n1, "Node Body 1"))
        dao.upsertNode(NodeEntity(2, n2, "Node Body 2"))
    }

    fun readEntity(e: ReadingEntity) = Reading(
        id = e.id,
        nodeId = e.node_id,
        numValue = e.num_value,
        temp = e.temp,
        humidity = e.humidity,
        ts = e.ts,
        dateStr = e.date_str,
        timeStr = e.time_str,
        nbcmMask = e.nbcm_mask,
        syncPending = e.isPending,
        source = e.source
    )

    companion object {
        fun dedupeKey(nodeId: Int, num: Int, ts: Long, temp: Float, hum: Float): String =
            "$nodeId|$num|$ts|${"%.1f".format(temp)}|${"%.1f".format(hum)}"

        fun tehranZone(): ZoneId = ZoneId.of("Asia/Tehran")
    }
}

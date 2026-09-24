package com.rftester.app.data.local

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import androidx.room.Transaction
import kotlinx.coroutines.flow.Flow

@Dao
interface ReadingDao {

    @Insert(onConflict = OnConflictStrategy.IGNORE)
    suspend fun insertAll(items: List<ReadingEntity>): List<Long>

    /** فقط ردیف‌های جدید (id تکراری نادیده گرفته می‌شود) */
    @Insert(onConflict = OnConflictStrategy.IGNORE)
    suspend fun insert(item: ReadingEntity): Long

    @Query("SELECT * FROM readings WHERE id = :id LIMIT 1")
    suspend fun getById(id: String): ReadingEntity?

    @Query("SELECT * FROM readings WHERE dedupe_key = :key LIMIT 1")
    suspend fun getByDedupeKey(key: String): ReadingEntity?

    @Query("SELECT * FROM readings ORDER BY ts DESC LIMIT :limit")
    suspend fun latest(limit: Int): List<ReadingEntity>

    @Query("SELECT * FROM readings ORDER BY ts DESC LIMIT :limit")
    fun latestFlow(limit: Int): Flow<List<ReadingEntity>>

    @Query("SELECT * FROM readings WHERE node_id = :nodeId ORDER BY ts DESC LIMIT :limit")
    fun latestForNodeFlow(nodeId: Int, limit: Int): Flow<List<ReadingEntity>>

    @Query("SELECT * FROM readings WHERE node_id = :nodeId AND ts BETWEEN :from AND :to ORDER BY ts ASC")
    suspend fun range(nodeId: Int, from: Long, to: Long): List<ReadingEntity>

    @Query("SELECT * FROM readings WHERE ts BETWEEN :from AND :to ORDER BY ts ASC")
    suspend fun rangeAll(from: Long, to: Long): List<ReadingEntity>

    @Query("SELECT * FROM readings WHERE node_id = :nodeId AND date_str = :date ORDER BY ts ASC")
    suspend fun byDate(nodeId: Int, date: String): List<ReadingEntity>

    @Query(
        """SELECT * FROM readings
           WHERE (:nodeId IS NULL OR node_id = :nodeId)
             AND (:q = '' OR date_str LIKE '%'||:q||'%' OR time_str LIKE '%'||:q||'%'
                  OR CAST(temp AS TEXT) LIKE '%'||:q||'%' OR CAST(humidity AS TEXT) LIKE '%'||:q||'%')
           ORDER BY ts DESC
           LIMIT :limit"""
    )
    suspend fun search(nodeId: Int?, q: String, limit: Int): List<ReadingEntity>

    @Query("SELECT COUNT(*) FROM readings")
    suspend fun count(): Long

    @Query("SELECT COUNT(*) FROM readings WHERE sync_state = 1")
    fun pendingCountFlow(): Flow<Int>

    @Query("SELECT COUNT(*) FROM readings WHERE sync_state = 1")
    suspend fun pendingCount(): Int

    @Query("SELECT * FROM readings WHERE sync_state = 1 ORDER BY ts ASC LIMIT :batch")
    suspend fun pendingBatch(batch: Int): List<ReadingEntity>

    @Query("UPDATE readings SET sync_state = 0 WHERE id IN (:ids)")
    suspend fun markSynced(ids: List<String>)

    @Query("DELETE FROM readings WHERE ts < :cutoff")
    suspend fun pruneOlderThan(cutoff: Long): Int

    @Query("DELETE FROM readings")
    suspend fun clearAll(): Int

    @Query("SELECT DISTINCT date_str FROM readings ORDER BY date_str DESC")
    suspend fun distinctDates(): List<String>

    @Query("SELECT DISTINCT date_str FROM readings ORDER BY date_str DESC")
    fun distinctDatesFlow(): Flow<List<String>>

    // ---------- آگرگیت‌های سبک برای تحلیل/نمودار (SQL، بدون کپی لیست) ----------

    @Query(
        """SELECT MIN(temp) AS mn, MAX(temp) AS mx, AVG(temp) AS av, COUNT(*) AS c
           FROM readings WHERE node_id = :nodeId AND ts BETWEEN :from AND :to"""
    )
    suspend fun tempStats(nodeId: Int, from: Long, to: Long): AggRow?

    @Query(
        """SELECT MIN(humidity) AS mn, MAX(humidity) AS mx, AVG(humidity) AS av, COUNT(*) AS c
           FROM readings WHERE node_id = :nodeId AND ts BETWEEN :from AND :to"""
    )
    suspend fun humidityStats(nodeId: Int, from: Long, to: Long): AggRow?

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsertNode(node: NodeEntity)

    @Query("SELECT * FROM nodes ORDER BY id ASC")
    fun nodesFlow(): Flow<List<NodeEntity>>

    @Query("SELECT * FROM nodes ORDER BY id ASC")
    suspend fun nodes(): List<NodeEntity>
}

data class AggRow(
    val mn: Float?,
    val mx: Float?,
    val av: Float?,
    val c: Int
)

@Dao
interface SyncDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsertMeta(meta: SyncMetaEntity)

    @Query("SELECT * FROM sync_meta WHERE id = 1")
    suspend fun meta(): SyncMetaEntity?

    @Query("SELECT * FROM sync_meta WHERE id = 1")
    fun metaFlow(): Flow<SyncMetaEntity?>
}

@Dao
interface PendingOpDao {
    @Insert
    suspend fun insert(op: PendingOpEntity): Long

    @Query("SELECT * FROM pending_ops ORDER BY created_at ASC LIMIT :limit")
    suspend fun batch(limit: Int): List<PendingOpEntity>

    @Query("DELETE FROM pending_ops WHERE id = :id")
    suspend fun delete(id: Long)

    @Query("UPDATE pending_ops SET retries = retries + 1 WHERE id = :id")
    suspend fun bumpRetry(id: Long)
}

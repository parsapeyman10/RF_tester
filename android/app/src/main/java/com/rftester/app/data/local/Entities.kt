package com.rftester.app.data.local

import androidx.room.Entity
import androidx.room.Index
import androidx.room.PrimaryKey

/**
 * جدول اصلی خوانش‌ها — طراحی «سبک + کامل»:
 *  - ts (epoch ms): ستون زمانی صحیح برای سورت/ایندکس/بازه
 *  - temp/humidity با تایپ REAL (نه رشته) → محاسبات دقیق و حجم کم
 *  - nbcm_mask: یک Integer به‌جای ۴ ستون متنی
 *  - sync_state: صف ارسال Store & Forward
 *  - ایندکس ترکیبی (node_id, ts) برای نمودارهای هر نود
 */
@Entity(
    tableName = "readings",
    indices = [
        Index(value = ["node_id", "ts"]),
        Index(value = ["ts"]),
        Index(value = ["sync_state"]),
        Index(value = ["date_str"]),
        Index(value = ["node_id", "date_str", "ts"])
    ]
)
data class ReadingEntity(
    @PrimaryKey val id: String,
    val node_id: Int,
    val num_value: Int,
    val temp: Float,
    val humidity: Float,
    val ts: Long,
    val date_str: String,
    val time_str: String,
    val nbcm_mask: Int,
    /** 1 = در صف ارسال به سرور ، 0 = ارسال‌شده */
    val sync_state: Int,
    val source: Int,
    /** کلید یکتای محتوایی برای جلوگیری از ثبت تکراری هنگام sync دوطرفه */
    val dedupe_key: String
) {
    val isPending: Boolean get() = sync_state == 1
}

/**
 * گرههای محصول (دو محصول نود بدنه).
 * برای تغییر نام از تنظیمات اپ استفاده می‌شود.
 */
@Entity(tableName = "nodes")
data class NodeEntity(
    @PrimaryKey val id: Int,
    val name: String,
    val subtitle: String,
    val enabled: Int = 1
)

/**
 * عملیات‌های معوق (مثلاً ارسال دسته‌ای). سبک و قابل بازسازی پس از ریست.
 */
@Entity(tableName = "pending_ops")
data class PendingOpEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val kind: String,
    val payload: String,
    val created_at: Long,
    val retries: Int = 0
)

/** وضعیت کلی همگام‌سازی (تک‌ردیفه) */
@Entity(tableName = "sync_meta")
data class SyncMetaEntity(
    @PrimaryKey val id: Int = 1,
    val last_sync_at: Long? = null,
    val last_error: String? = null,
    val last_server_ip: String? = null
)

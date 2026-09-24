package com.rftester.app.data.sync

import android.content.Context
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import com.rftester.app.RfApp
import java.util.concurrent.TimeUnit

/**
 * ارسال خودکار داده‌های ذخیره‌شده آفلاین به‌محض برقراری اتصال
 * + همگام‌سازی دوره‌ای هر ۱۵ دقیقه.
 */
class SyncWorker(
    context: Context,
    params: WorkerParameters
) : CoroutineWorker(context, params) {

    override suspend fun doWork(): Result {
        val app = applicationContext as? RfApp ?: return Result.success()
        return try {
            val (pushed, pulled) = app.container.readingRepository.syncNow()
            // کاری نبود یا موفق بود → موفق؛ خطای شبکه → تلاش مجدد بعداً
            Result.success()
        } catch (t: Throwable) {
            Result.retry()
        }
    }

    companion object {
        const val UNIQUE = "rf_sync_periodic"

        fun schedule(context: Context) {
            val constraints = Constraints.Builder()
                .setRequiredNetworkType(NetworkType.CONNECTED)
                .build()

            val request = PeriodicWorkRequestBuilder<SyncWorker>(15, TimeUnit.MINUTES)
                .setConstraints(constraints)
                .build()

            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                UNIQUE,
                ExistingPeriodicWorkPolicy.KEEP, // تنها یک نمونه زنده
                request
            )
        }

        fun onceNow(context: Context) {
            val constraints = Constraints.Builder()
                .setRequiredNetworkType(NetworkType.CONNECTED)
                .build()
            androidx.work.OneTimeWorkRequestBuilder<SyncWorker>()
                .setConstraints(constraints)
                .build()
                .let {
                    WorkManager.getInstance(context).enqueueUniqueWork(
                        "${UNIQUE}_now",
                        androidx.work.ExistingWorkPolicy.REPLACE,
                        it
                    )
                }
        }
    }
}

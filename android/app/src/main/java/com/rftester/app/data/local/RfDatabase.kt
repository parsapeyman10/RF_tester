package com.rftester.app.data.local

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.sqlite.db.SupportSQLiteDatabase
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

@Database(
    entities = [
        ReadingEntity::class,
        NodeEntity::class,
        PendingOpEntity::class,
        SyncMetaEntity::class
    ],
    version = 1,
    exportSchema = false
)
abstract class RfDatabase : RoomDatabase() {
    abstract fun readingDao(): ReadingDao
    abstract fun syncDao(): SyncDao
    abstract fun pendingOpDao(): PendingOpDao

    companion object {
        @Volatile
        private var instance: RfDatabase? = null

        fun get(context: Context): RfDatabase =
            instance ?: synchronized(this) {
                instance ?: build(context).also { instance = it }
            }

        private fun build(context: Context): RfDatabase =
            Room.databaseBuilder(
                context.applicationContext,
                RfDatabase::class.java,
                "rf_tester.db"
            )
                // WAL → خواندن روان هنگام نوشتن (نمودار زنده بدون لگ)
                .setJournalMode(androidx.room.RoomDatabase.JournalMode.WRITE_AHEAD_LOGGING)
                .addCallback(object : RoomDatabase.Callback() {
                    override fun onCreate(db: SupportSQLiteDatabase) {
                        super.onCreate(db)
                        CoroutineScope(Dispatchers.IO).launch {
                            get(context).readingDao().upsertNode(
                                NodeEntity(1, "نود بدنه ۱", "Node Body 1")
                            )
                            get(context).readingDao().upsertNode(
                                NodeEntity(2, "نود بدنه ۲", "Node Body 2")
                            )
                            get(context).syncDao().upsertMeta(SyncMetaEntity())
                        }
                    }
                })
                .build()
    }
}

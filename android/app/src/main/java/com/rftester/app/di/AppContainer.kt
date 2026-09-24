package com.rftester.app.di

import android.content.Context
import com.rftester.app.data.local.RfDatabase
import com.rftester.app.data.repo.ReadingRepository
import com.rftester.app.data.repo.SettingsRepository

/** DI سبک دستی — بدون Hilt (کمترین وابستگی، بیلد سریع‌تر) */
class AppContainer(context: Context) {
    val settings: SettingsRepository = SettingsRepository(context)
    val database: RfDatabase = RfDatabase.get(context)
    val readingRepository: ReadingRepository =
        ReadingRepository(context, database, settings)
}

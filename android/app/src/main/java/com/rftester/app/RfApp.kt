package com.rftester.app

import android.app.Application
import com.rftester.app.data.sync.SyncWorker
import com.rftester.app.di.AppContainer

class RfApp : Application() {

    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        container = AppContainer(this)
        // ذخیره آفلاین + ارسال خودکار بعد از اتصال
        SyncWorker.schedule(this)
    }

    companion object {
        lateinit var instance: RfApp
            private set
    }
}

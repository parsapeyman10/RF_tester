package com.rftester.app.data.repo

import android.content.Context
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import okhttp3.OkHttpClient
import retrofit2.Retrofit
import retrofit2.converter.kotlinxserialization.asConverterFactory
import com.rftester.app.data.remote.RfApi
import kotlinx.serialization.json.Json
import okhttp3.MediaType.Companion.toMediaType
import java.util.concurrent.TimeUnit

/**
 * تنظیمات + ساخت کلاینت شبکه.
 * آدرس سرور از تنظیمات خوانده می‌شود (پیش‌فرض: همان Flask موجود).
 */
class SettingsRepository(context: Context) {

    private val masterKey = MasterKey.Builder(context)
        .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
        .build()

    private val prefs = EncryptedSharedPreferences.create(
        context,
        "rf_settings",
        masterKey,
        EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
        EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
    )

    var serverUrl: String
        get() = prefs.getString(KEY_SERVER, DEFAULT_SERVER) ?: DEFAULT_SERVER
        set(value) = prefs.edit().putString(KEY_SERVER, value.trim().trimEnd('/')).apply()

    var node1Name: String
        get() = prefs.getString(KEY_N1, "نود بدنه ۱") ?: "نود بدنه ۱"
        set(value) = prefs.edit().putString(KEY_N1, value).apply()

    var node2Name: String
        get() = prefs.getString(KEY_N2, "نود بدنه ۲") ?: "نود بدنه ۲"
        set(value) = prefs.edit().putString(KEY_N2, value).apply()

    var darkMode: Boolean
        get() = prefs.getBoolean(KEY_DARK, true)
        set(value) = prefs.edit().putBoolean(KEY_DARK, value).apply()

    var retainDays: Int
        get() = prefs.getInt(KEY_RETAIN, 365)
        set(value) = prefs.edit().putInt(KEY_RETAIN, value).apply()

    fun api(baseUrl: String = serverUrl): RfApi {
        val normalized = if (baseUrl.endsWith("/")) baseUrl else "$baseUrl/"
        val json = Json {
            ignoreUnknownKeys = true
            isLenient = true
            coerceInputValues = true
        }
        val client = OkHttpClient.Builder()
            .connectTimeout(6, TimeUnit.SECONDS)
            .readTimeout(12, TimeUnit.SECONDS)
            .writeTimeout(12, TimeUnit.SECONDS)
            .retryOnConnectionFailure(true)
            .build()

        return Retrofit.Builder()
            .baseUrl(normalized)
            .client(client)
            .addConverterFactory(json.asConverterFactory("application/json".toMediaType()))
            .build()
            .create(RfApi::class.java)
    }

    companion object {
        const val DEFAULT_SERVER = "http://192.168.1.10:5000"
        private const val KEY_SERVER = "server_url"
        private const val KEY_N1 = "node1_name"
        private const val KEY_N2 = "node2_name"
        private const val KEY_DARK = "dark_mode"
        private const val KEY_RETAIN = "retain_days"
    }
}

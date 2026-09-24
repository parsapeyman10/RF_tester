package com.rftester.app.data.remote

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import retrofit2.Response
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Query

/** خروجی سازگار با /api/master_data سرور Flask */
@Serializable
data class ServerReading(
    val num_value: Int? = null,
    val temp: String? = null,
    val humidity: String? = null,
    val timestamp: String? = null,   // 2026-01-01T12:30:45  یا 2026-01-01 12:30:45
    val date: String? = null,
    val time: String? = null,
    val node_id: Int? = null,
    val nbcm_statuses: Map<String, String>? = null
)

/** بچ ارسالی اپ به سرور (Store & Forward) */
@Serializable
data class IngestRequest(
    val device: String = "android",
    val readings: List<IngestItem>
)

@Serializable
data class IngestItem(
    @SerialName("id") val id: String,
    @SerialName("node_id") val nodeId: Int,
    @SerialName("num_value") val numValue: Int,
    val temp: Float,
    val humidity: Float,
    @SerialName("ts") val ts: Long,
    @SerialName("date") val date: String,
    @SerialName("time") val time: String,
    @SerialName("nbcm_mask") val nbcmMask: Int,
    val source: Int
)

@Serializable
data class IngestResponse(
    val status: String? = null,
    val accepted: Int? = null,
    val duplicates: Int? = null,
    val message: String? = null
)

@Serializable
data class HealthResponse(
    val status: String? = null,
    val records: Long? = null,
    val time: String? = null
)

interface RfApi {

    @GET("api/master_data")
    suspend fun masterData(
        @Query("start_date") startDate: String? = null,
        @Query("end_date") endDate: String? = null,
        @Query("limit") limit: Int? = null
    ): Response<List<ServerReading>>

    @GET("api/sensor_data")
    suspend fun sensorData(): Response<List<ServerReading>>

    @POST("api/ingest")
    suspend fun ingest(@Body body: IngestRequest): Response<IngestResponse>

    @GET("api/app_health")
    suspend fun health(): Response<HealthResponse>
}

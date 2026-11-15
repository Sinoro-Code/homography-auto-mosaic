package com.antigravity.homographymosaic.core

import android.content.Context
import androidx.camera.core.ImageProxy
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.util.UUID

data class FamTemplateEntry(
    val id: String,
    val displayName: String,
    val famTplFilePath: String,
    val fileBytes: Long,
    val keypointCount: Int,
    val polygonCount: Int,
    val vertexCount: Int,
    val enabled: Boolean,
    val createdEpochMs: Long,
    val lastMmapLoadUs: Long = 0L
)

/**
 * Manages the lifecycle of FAMTPL02 binary templates stored under `filesDir/templates/<id>.famtpl`.
 * Performs Sub-1ms cold-start memory-mapped (mmap) preloading without decoding reference images.
 */
class TemplateRepository(
    context: Context,
    private val engineSession: MosaicEngineSession
) {
    private val templatesDir: File = File(context.filesDir, "templates").apply { mkdirs() }

    private val _templates = MutableStateFlow<List<FamTemplateEntry>>(emptyList())
    val templates: StateFlow<List<FamTemplateEntry>> = _templates.asStateFlow()

    private val _coldStartTotalUs = MutableStateFlow(0L)
    val coldStartTotalUs: StateFlow<Long> = _coldStartTotalUs.asStateFlow()

    /**
     * Cold-start preload: memory-maps all enabled *.famtpl files directly into the C++20 engine.
     */
    suspend fun preloadAllTemplates(): Long = withContext(Dispatchers.IO) {
        val tplFiles = templatesDir
            .listFiles { f -> f.isFile && f.extension == "famtpl" }
            ?.sortedByDescending { it.lastModified() }
            .orEmpty()

        engineSession.clearAllTemplates()

        var accumulatedLoadUs = 0L
        val loadedEntries = ArrayList<FamTemplateEntry>(tplFiles.size)

        for (tplFile in tplFiles) {
            val id = tplFile.nameWithoutExtension
            val metaFile = File(templatesDir, "$id.meta.json")
            val meta = readMetadataOrFallback(id, tplFile, metaFile)

            if (meta.enabled) {
                val loadTel = engineSession.loadTemplateMmap(id, tplFile.absolutePath)
                if (loadTel.success) {
                    accumulatedLoadUs += loadTel.loadElapsedUs
                    loadedEntries.add(meta.copy(lastMmapLoadUs = loadTel.loadElapsedUs))
                }
            } else {
                loadedEntries.add(meta)
            }
        }

        _templates.value = loadedEntries
        _coldStartTotalUs.value = accumulatedLoadUs
        accumulatedLoadUs
    }

    /**
     * Learns a new FAMTPL02 template directly from a CameraX YUV_420_888 frame's Y plane
     * (zero Bitmap, zero RGB) and atomically persists both .famtpl and .meta.json.
     */
    suspend fun learnAndSaveTemplate(
        displayName: String,
        image: ImageProxy,
        normalizedPolygons: List<List<NormalizedPoint>>
    ): Result<FamTemplateEntry> = withContext(Dispatchers.Default) {
        val cleanPolys = normalizedPolygons.filter { it.size >= 3 }
        if (cleanPolys.isEmpty()) {
            return@withContext Result.failure(IllegalArgumentException("請至少框選一個至少 3 頂點的多邊形遮罩區域"))
        }

        val id = "tpl_${System.currentTimeMillis()}_${UUID.randomUUID().toString().take(6)}"
        val tplFile = File(templatesDir, "$id.famtpl")
        val metaFile = File(templatesDir, "$id.meta.json")

        val tel = engineSession.learnTemplateFromImageProxy(
            image = image,
            normalizedPolygons = cleanPolys,
            outputTplPath = tplFile.absolutePath,
            templateId = id
        )

        if (!tel.success || !tplFile.exists()) {
            tplFile.delete()
            return@withContext Result.failure(
                IllegalStateException("特徵點不足（請對準具文字或邊角紋理之目標並保持防手震穩定）")
            )
        }

        val nowMs = System.currentTimeMillis()
        val entry = FamTemplateEntry(
            id = id,
            displayName = displayName.ifBlank { "自學習範本 #${_templates.value.size + 1}" },
            famTplFilePath = tplFile.absolutePath,
            fileBytes = tplFile.length(),
            keypointCount = tel.extractedKeypoints,
            polygonCount = cleanPolys.size,
            vertexCount = cleanPolys.sumOf { it.size },
            enabled = true,
            createdEpochMs = nowMs,
            lastMmapLoadUs = tel.elapsedUs
        )

        writeMetadataAtomically(metaFile, entry)
        _templates.value = listOf(entry) + _templates.value
        Result.success(entry)
    }

    suspend fun setTemplateEnabled(id: String, enabled: Boolean) = withContext(Dispatchers.IO) {
        val current = _templates.value.toMutableList()
        val index = current.indexOfFirst { it.id == id }
        if (index < 0) return@withContext

        val target = current[index]
        var mmapUs = target.lastMmapLoadUs
        if (enabled) {
            val tel = engineSession.loadTemplateMmap(id, target.famTplFilePath)
            if (!tel.success) return@withContext
            mmapUs = tel.loadElapsedUs
        } else {
            engineSession.unloadTemplate(id)
        }

        val updated = target.copy(enabled = enabled, lastMmapLoadUs = mmapUs)
        writeMetadataAtomically(File(templatesDir, "$id.meta.json"), updated)
        current[index] = updated
        _templates.value = current
    }

    suspend fun deleteTemplate(id: String) = withContext(Dispatchers.IO) {
        engineSession.unloadTemplate(id)
        File(templatesDir, "$id.famtpl").delete()
        File(templatesDir, "$id.meta.json").delete()
        _templates.value = _templates.value.filterNot { it.id == id }
    }

    private fun readMetadataOrFallback(id: String, tplFile: File, metaFile: File): FamTemplateEntry {
        if (metaFile.exists()) {
            runCatching {
                val json = JSONObject(metaFile.readText(Charsets.UTF_8))
                return FamTemplateEntry(
                    id = id,
                    displayName = json.optString("displayName", id),
                    famTplFilePath = tplFile.absolutePath,
                    fileBytes = tplFile.length(),
                    keypointCount = json.optInt("keypointCount", 0),
                    polygonCount = json.optInt("polygonCount", 1),
                    vertexCount = json.optInt("vertexCount", 4),
                    enabled = json.optBoolean("enabled", true),
                    createdEpochMs = json.optLong("createdEpochMs", tplFile.lastModified())
                )
            }
        }
        return FamTemplateEntry(
            id = id,
            displayName = id,
            famTplFilePath = tplFile.absolutePath,
            fileBytes = tplFile.length(),
            keypointCount = 0,
            polygonCount = 1,
            vertexCount = 4,
            enabled = true,
            createdEpochMs = tplFile.lastModified()
        )
    }

    private fun writeMetadataAtomically(metaFile: File, entry: FamTemplateEntry) {
        val tmp = File(metaFile.parentFile, "${metaFile.name}.tmp")
        val json = JSONObject().apply {
            put("id", entry.id)
            put("displayName", entry.displayName)
            put("keypointCount", entry.keypointCount)
            put("polygonCount", entry.polygonCount)
            put("vertexCount", entry.vertexCount)
            put("enabled", entry.enabled)
            put("createdEpochMs", entry.createdEpochMs)
        }
        tmp.writeText(json.toString(2), Charsets.UTF_8)
        tmp.renameTo(metaFile)
    }
}

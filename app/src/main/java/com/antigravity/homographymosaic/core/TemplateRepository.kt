package com.antigravity.homographymosaic.core

import android.content.Context
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
     * Learns a new FAMTPL02 template directly from a gallery JPEG file descriptor in YUV420 space
     * and atomically persists both .famtpl and .meta.json.
     */
    suspend fun learnAndSaveTemplateFromJpegFd(
        displayName: String,
        inFd: Int,
        uprightPolygons: List<List<NormalizedPoint>>,
        makeExclusiveActive: Boolean = true
    ): Result<FamTemplateEntry> = withContext(Dispatchers.IO) {
        val cleanPolys = uprightPolygons.filter { it.size >= 3 }
        if (cleanPolys.isEmpty()) {
            return@withContext Result.failure(
                IllegalArgumentException("請先在相片上塗抹或點擊框選至少一個打碼位置（至少 3 個頂點）")
            )
        }

        val id = "tpl_${System.currentTimeMillis()}_${UUID.randomUUID().toString().take(6)}"
        val tplFile = File(templatesDir, "$id.famtpl")
        val metaFile = File(templatesDir, "$id.meta.json")

        val tel = engineSession.learnTemplateFromJpegFd(
            inFd = inFd,
            uprightPolygons = cleanPolys,
            outputTplPath = tplFile.absolutePath,
            templateId = id
        )

        if (!tel.success || !tplFile.exists()) {
            tplFile.delete()
            return@withContext Result.failure(
                IllegalStateException("無法解析所選相片（支援 JPEG / HEIC / HEIF / PNG / WebP / AVIF，請確認檔案未損毀）")
            )
        }

        val updatedPrevious = if (makeExclusiveActive) {
            _templates.value.map { existing ->
                if (existing.enabled) {
                    engineSession.unloadTemplate(existing.id)
                    val disabled = existing.copy(enabled = false)
                    writeMetadataAtomically(File(templatesDir, "${existing.id}.meta.json"), disabled)
                    disabled
                } else {
                    existing
                }
            }
        } else {
            _templates.value
        }

        val nowMs = System.currentTimeMillis()
        val entry = FamTemplateEntry(
            id = id,
            displayName = displayName.ifBlank { "打碼位置 #${updatedPrevious.size + 1}" },
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
        _templates.value = listOf(entry) + updatedPrevious
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

package com.antigravity.homographymosaic

import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.antigravity.homographymosaic.core.MosaicEngineSession
import com.antigravity.homographymosaic.core.TemplateRepository
import com.antigravity.homographymosaic.ui.BatchGalleryScreen
import com.antigravity.homographymosaic.ui.PhotoLearnScreen

private val TacticalDarkColorScheme = darkColorScheme(
    primary = Color(0xFF00E5A3),
    secondary = Color(0xFF38BDF8),
    background = Color(0xFF0B0F17),
    surface = Color(0xFF111827),
    onPrimary = Color(0xFF0B0F17),
    onSecondary = Color(0xFF0B0F17),
    onBackground = Color(0xFFF1F5F9),
    onSurface = Color(0xFFF1F5F9)
)

class MainActivity : ComponentActivity() {

    private lateinit var engineSession: MosaicEngineSession
    private lateinit var templateRepository: TemplateRepository

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.parseColor("#0B0F17"))
        )
        super.onCreate(savedInstanceState)

        engineSession = MosaicEngineSession(
            initialBlockSize = 32,
            initialNoiseAmplitude = 18
        )
        templateRepository = TemplateRepository(applicationContext, engineSession)

        setContent {
            MaterialTheme(colorScheme = TacticalDarkColorScheme) {
                HomographyMosaicApp(
                    engineSession = engineSession,
                    templateRepository = templateRepository
                )
            }
        }
    }

    override fun onDestroy() {
        if (::engineSession.isInitialized) {
            engineSession.close()
        }
        super.onDestroy()
    }
}

@Composable
private fun HomographyMosaicApp(
    engineSession: MosaicEngineSession,
    templateRepository: TemplateRepository
) {
    var learningPhotoUriString by rememberSaveable { mutableStateOf<String?>(null) }

    // Sub-1ms FAMTPL02 binary mmap cold-start preload (zero camera permissions needed)
    LaunchedEffect(Unit) {
        templateRepository.preloadAllTemplates()
    }

    Scaffold(
        containerColor = Color(0xFF0B0F17),
        topBar = {
            Surface(
                color = Color(0xFF0F172A),
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(
                    modifier = Modifier
                        .statusBarsPadding()
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = if (learningPhotoUriString != null) "建立範本" else "自碼系統",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold,
                        color = Color.White
                    )
                }
            }
        }
    ) { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .navigationBarsPadding()
                .background(Color(0xFF0B0F17))
        ) {
            val activeLearnUri = learningPhotoUriString?.let { Uri.parse(it) }
            if (activeLearnUri != null) {
                PhotoLearnScreen(
                    initialReferenceUri = activeLearnUri,
                    templateRepository = templateRepository,
                    onCancel = { learningPhotoUriString = null },
                    onLearnedSuccess = { learningPhotoUriString = null }
                )
            } else {
                BatchGalleryScreen(
                    engineSession = engineSession,
                    templateRepository = templateRepository,
                    onReferencePhotoPicked = { pickedUri ->
                        learningPhotoUriString = pickedUri.toString()
                    }
                )
            }
        }
    }
}

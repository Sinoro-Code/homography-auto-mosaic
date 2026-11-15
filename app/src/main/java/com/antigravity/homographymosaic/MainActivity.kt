package com.antigravity.homographymosaic

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.PhotoLibrary
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.antigravity.homographymosaic.core.MosaicEngineSession
import com.antigravity.homographymosaic.core.TemplateRepository
import com.antigravity.homographymosaic.ui.BatchGalleryScreen
import com.antigravity.homographymosaic.ui.CameraCaptureScreen

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

private enum class AppDestination {
    CAMERA_CAPTURE_AND_LEARN,
    BATCH_GALLERY
}

class MainActivity : ComponentActivity() {

    private lateinit var engineSession: MosaicEngineSession
    private lateinit var templateRepository: TemplateRepository

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.parseColor("#0B0F17"))
        )
        super.onCreate(savedInstanceState)

        engineSession = MosaicEngineSession(initialBlockSize = 16, initialNoiseAmplitude = 18)
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
    val context = LocalContext.current

    var currentTab by rememberSaveable { mutableStateOf(AppDestination.CAMERA_CAPTURE_AND_LEARN) }
    var blockSize by rememberSaveable { mutableIntStateOf(16) }
    var jpegQuality by rememberSaveable { mutableIntStateOf(92) }

    var hasCameraPermission by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED
        )
    }

    val permissionLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.RequestPermission()
    ) { granted ->
        hasCameraPermission = granted
    }

    // Sub-1ms FAMTPL02 binary mmap cold-start preload
    LaunchedEffect(Unit) {
        templateRepository.preloadAllTemplates()
        if (!hasCameraPermission) {
            permissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    val coldStartUs by templateRepository.coldStartTotalUs.collectAsState()
    val templates by templateRepository.templates.collectAsState()

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
                        .padding(horizontal = 16.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Column {
                        Text(
                            text = "Homography Auto-Mosaic",
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.Bold,
                            color = Color.White
                        )
                        Text(
                            text = "Pure CPU NEON+POPCNT · YUV420 In-Place · Zero Bitmap",
                            style = MaterialTheme.typography.labelSmall,
                            color = Color(0xFF94A3B8)
                        )
                    }

                    Surface(
                        shape = RoundedCornerShape(999.dp),
                        color = Color(0xFF00E5A3).copy(alpha = 0.14f)
                    ) {
                        Row(
                            modifier = Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Icon(
                                Icons.Default.Memory,
                                contentDescription = null,
                                tint = Color(0xFF00E5A3),
                                modifier = Modifier.padding(end = 4.dp)
                            )
                            val msText = "%.3f".format(coldStartUs / 1000.0)
                            Text(
                                text = "FAMTPL02(${templates.count { it.enabled }}) ${msText}ms",
                                style = MaterialTheme.typography.labelSmall,
                                fontWeight = FontWeight.SemiBold,
                                color = Color(0xFF00E5A3)
                            )
                        }
                    }
                }
            }
        },
        bottomBar = {
            NavigationBar(containerColor = Color(0xFF0F172A)) {
                NavigationBarItem(
                    selected = currentTab == AppDestination.CAMERA_CAPTURE_AND_LEARN,
                    onClick = { currentTab = AppDestination.CAMERA_CAPTURE_AND_LEARN },
                    icon = { Icon(Icons.Default.CameraAlt, contentDescription = null) },
                    label = { Text("相機擷取與學習") },
                    colors = NavigationBarItemDefaults.colors(
                        selectedIconColor = Color(0xFF00E5A3),
                        selectedTextColor = Color(0xFF00E5A3),
                        indicatorColor = Color(0xFF00E5A3).copy(alpha = 0.16f),
                        unselectedIconColor = Color(0xFF64748B),
                        unselectedTextColor = Color(0xFF64748B)
                    )
                )
                NavigationBarItem(
                    selected = currentTab == AppDestination.BATCH_GALLERY,
                    onClick = { currentTab = AppDestination.BATCH_GALLERY },
                    icon = { Icon(Icons.Default.PhotoLibrary, contentDescription = null) },
                    label = { Text("相簿極速批次打碼") },
                    colors = NavigationBarItemDefaults.colors(
                        selectedIconColor = Color(0xFF38BDF8),
                        selectedTextColor = Color(0xFF38BDF8),
                        indicatorColor = Color(0xFF38BDF8).copy(alpha = 0.16f),
                        unselectedIconColor = Color(0xFF64748B),
                        unselectedTextColor = Color(0xFF64748B)
                    )
                )
            }
        }
    ) { innerPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .background(Color(0xFF0B0F17))
        ) {
            when (currentTab) {
                AppDestination.CAMERA_CAPTURE_AND_LEARN -> {
                    if (hasCameraPermission) {
                        CameraCaptureScreen(
                            engineSession = engineSession,
                            templateRepository = templateRepository,
                            blockSize = blockSize,
                            jpegQuality = jpegQuality
                        )
                    } else {
                        CameraPermissionPrompt(
                            onRequestPermission = {
                                permissionLauncher.launch(Manifest.permission.CAMERA)
                            }
                        )
                    }
                }

                AppDestination.BATCH_GALLERY -> {
                    BatchGalleryScreen(
                        engineSession = engineSession,
                        templateRepository = templateRepository,
                        blockSize = blockSize,
                        onBlockSizeChange = { blockSize = it },
                        jpegQuality = jpegQuality,
                        onJpegQualityChange = { jpegQuality = it }
                    )
                }
            }
        }
    }
}

@Composable
private fun CameraPermissionPrompt(onRequestPermission: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = "需要相機權限以啟用 YUV_420_888 零拷貝擷取",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
            color = Color.White
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = "相機影像僅於本機 YUV420 記憶體平面原地運算，不產生 Bitmap、不上傳任何外部伺服器。",
            style = MaterialTheme.typography.bodySmall,
            color = Color(0xFF94A3B8)
        )
        Spacer(modifier = Modifier.height(20.dp))
        Button(
            onClick = onRequestPermission,
            colors = ButtonDefaults.buttonColors(
                containerColor = Color(0xFF00E5A3),
                contentColor = Color(0xFF0B0F17)
            )
        ) {
            Text("授予相機權限", fontWeight = FontWeight.Bold)
        }
    }
}

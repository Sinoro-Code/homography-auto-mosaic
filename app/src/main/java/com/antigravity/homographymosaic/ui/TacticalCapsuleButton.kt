package com.antigravity.homographymosaic.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ProvideTextStyle
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.compositeOver
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp

val TacticalCapsuleShape = CircleShape
val TacticalBaseBg = Color(0xFF0A0F17).copy(alpha = 0.86f)
val TacticalAccentPrimary = Color(0xFF17BF82)
val TacticalAccentHover = Color(0xFF24D695)
val TacticalTextPrimary = Color(0xFFF8FAFC)

@Composable
fun tacticalCapsuleColors(
    pressed: Boolean = false,
    selected: Boolean = false,
    inactiveGray: Boolean = false
): Pair<Color, Color> {
    if (inactiveGray) {
        val overlayAlpha = if (pressed) 0.42f else 0.30f
        val bg = Color(0xFF475569).copy(alpha = overlayAlpha).compositeOver(TacticalBaseBg)
        val border = if (pressed) Color(0xFF94A3B8) else Color(0xFF64748B)
        return bg to border
    }
    val overlayAlpha = if (pressed || selected) 0.38f else 0.26f
    val bg = TacticalAccentPrimary.copy(alpha = overlayAlpha).compositeOver(TacticalBaseBg)
    val border = if (pressed || selected) TacticalAccentHover else TacticalAccentPrimary
    return bg to border
}

@Composable
fun TacticalCapsuleButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    selected: Boolean = false,
    inactiveGray: Boolean = false,
    contentPadding: PaddingValues? = null,
    content: @Composable () -> Unit
) {
    val isCompact = LocalConfiguration.current.screenWidthDp <= 380
    val resolvedPadding = contentPadding ?: if (isCompact) {
        PaddingValues(horizontal = 16.dp, vertical = 9.dp)
    } else {
        PaddingValues(horizontal = 22.dp, vertical = 10.dp)
    }
    val fontSize = if (isCompact) 13.sp else 14.sp

    val interactionSource = remember { MutableInteractionSource() }
    val isPressed by interactionSource.collectIsPressedAsState()
    val scale by animateFloatAsState(
        targetValue = if (isPressed && enabled) 0.97f else 1f,
        animationSpec = tween(durationMillis = 120),
        label = "capsuleScale"
    )
    val (bgColor, borderColor) = tacticalCapsuleColors(
        pressed = isPressed && enabled,
        selected = selected,
        inactiveGray = inactiveGray
    )
    val contentColor = if (enabled) TacticalTextPrimary else TacticalTextPrimary.copy(alpha = 0.45f)

    Surface(
        shape = TacticalCapsuleShape,
        color = bgColor,
        contentColor = contentColor,
        border = BorderStroke(1.dp, borderColor),
        shadowElevation = 10.dp,
        modifier = modifier.graphicsLayer {
            scaleX = scale
            scaleY = scale
            alpha = if (enabled) 1f else 0.6f
        }
    ) {
        Box(
            modifier = Modifier
                .clickable(
                    interactionSource = interactionSource,
                    indication = null,
                    enabled = enabled,
                    role = Role.Button,
                    onClick = onClick
                )
                .padding(resolvedPadding),
            contentAlignment = Alignment.Center
        ) {
            ProvideTextStyle(
                value = TextStyle(
                    fontSize = fontSize,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 0.03.em,
                    color = contentColor
                )
            ) {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    content()
                }
            }
        }
    }
}

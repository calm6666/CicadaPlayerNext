package com.cicada.player.compose.ui.theme

import android.app.Activity
import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

/**
 * 应用主题。
 *
 * 1. 主色 = 品牌色（深 `#D44E7D` / 浅 `#FF6699`，见 Color.kt）；
 * 2. **默认关掉动态取色**：Material You 会拿壁纸色覆盖 primary，品牌主题色就丢了；
 * 3. 顺便把播放器控制层那套固定色（不跟主题走的白色系 + 黑遮罩）通过
 *    `LocalPlayerColors` 下发，界面取色统一走 `PlayerTheme`。
 */
private val DarkColorScheme = darkColorScheme(
    primary = BrandAccentDark,
    onPrimary = Color.White,
    primaryContainer = BrandAccentActiveDark,
    onPrimaryContainer = Color.White,
    tertiary = BrandAccentLight,
    onTertiary = Color.White,
    background = AppWindowBgDark,
    onBackground = AppTextDark,
    surface = AppPanelBgDark,
    onSurface = AppTextDark,
    surfaceVariant = AppPanelBorderDark,
    outline = AppPanelBorderDark,
)

private val LightColorScheme = lightColorScheme(
    primary = BrandAccentLight,
    onPrimary = Color.White,
    primaryContainer = BrandAccentActiveLight,
    onPrimaryContainer = Color.White,
    tertiary = BrandAccentDark,
    onTertiary = Color.White,
    background = AppWindowBgLight,
    onBackground = AppTextLight,
    surface = AppPanelBgLight,
    onSurface = AppTextLight,
    surfaceVariant = AppPanelBorderLight,
    outline = AppPanelBorderLight,
)

@Composable
fun CicadaPlayerTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    /** 动态取色默认关闭（见上面第 2 条） */
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit,
) {
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }

        darkTheme -> DarkColorScheme
        else -> LightColorScheme
    }

    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            WindowCompat.getInsetsController(window, view)
                .isAppearanceLightStatusBars = !darkTheme
        }
    }

    CompositionLocalProvider(LocalPlayerColors provides playerColors(darkTheme)) {
        MaterialTheme(
            colorScheme = colorScheme,
            typography = Typography,
            content = content,
        )
    }
}

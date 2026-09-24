package com.cicada.player.compose.components.danmaku

import androidx.compose.ui.graphics.Color
import java.util.UUID

data class Danmaku(
    val id: String = UUID.randomUUID().toString(),
    val text: String,
    val color: Color = Color.White,
    val textSize: Float = 32f,
    val type: DanmakuType = DanmakuType.SCROLL,
    val position: Float = 0f,
    val time: Long = 0L,
    var x: Float = 0f,
    var y: Float = 0f,
    var alpha: Float = 1f,
    var speed: Float = 1f,
    var isMoving: Boolean = true
)

enum class DanmakuType {
    SCROLL,    // 滚动弹幕
    TOP,       // 顶部固定弹幕
    BOTTOM     // 底部固定弹幕
}

enum class DanmakuSpeed {
    SLOW,
    NORMAL,
    FAST,
    VERY_FAST;

    val multiplier: Float
        get() = when (this) {
            SLOW -> 0.8f
            NORMAL -> 1.0f
            FAST -> 1.2f
            VERY_FAST -> 1.5f
        }
}

data class DanmakuConfig(
    var isEnabled: Boolean = true,
    var speed: DanmakuSpeed = DanmakuSpeed.NORMAL,
    var textSize: Float = 32f,
    var opacity: Float = 1f,
    var maxLines: Int = 8,
    var showTop: Boolean = true,
    var showBottom: Boolean = true,
    var showScroll: Boolean = true
)
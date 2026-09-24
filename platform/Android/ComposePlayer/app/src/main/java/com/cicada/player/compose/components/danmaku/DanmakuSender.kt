package com.cicada.player.compose.components.danmaku

import androidx.compose.animation.*
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cicada.player.compose.components.danmaku.Danmaku
import com.cicada.player.compose.components.danmaku.DanmakuType

@Composable
fun DanmakuSender(
    currentTime: Long,
    onSendDanmaku: (Danmaku) -> Unit = {},
    isDarkMode: Boolean = false,
    isFullscreen: Boolean = false
) {
    var text by remember { mutableStateOf("") }
    var selectedColor by remember { mutableStateOf(Color.White) }
    var selectedType by remember { mutableStateOf(DanmakuType.SCROLL) }
    var showColorPicker by remember { mutableStateOf(false) }
    var showTypeSelector by remember { mutableStateOf(false) }

    // 预定义颜色选项
    val colorOptions = listOf(
        Color.White,
        Color.Red,
        Color.Green,
        Color.Blue,
        Color.Yellow,
        Color.Magenta,
        Color.Cyan,
        Color(0xFFFFA500), // 橙色
        Color(0xFF800080), // 紫色
        Color(0xFF008000)  // 深绿色
    )

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .background(
                color = if (isDarkMode) Color.Black.copy(alpha = 0.8f) else Color.White.copy(alpha = 0.9f)
            )
            .padding(16.dp)
    ) {
        Column {
            // 主输入区域
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(if (isFullscreen) 8.dp else 4.dp)
            ) {
                // 颜色选择按钮
                Box(
                    modifier = Modifier
                        .size(40.dp)
                        .clip(CircleShape)
                        .background(selectedColor)
                        .border(
                            width = 2.dp,
                            color = Color(0xFFFB7299),
                            shape = CircleShape
                        )
                        .clickable { showColorPicker = !showColorPicker },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        Icons.Default.Settings,
                        contentDescription = "选择颜色",
                        tint = if (selectedColor == Color.White) Color.Black else Color.White,
                        modifier = Modifier.size(20.dp)
                    )
                }

                // 弹幕类型选择按钮
                Button(
                    onClick = { showTypeSelector = !showTypeSelector },
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (selectedType == DanmakuType.SCROLL)
                            Color(0xFFFB7299) else Color(0xFF666666),
                        contentColor = Color.White
                    ),
                    shape = RoundedCornerShape(20.dp),
                    modifier = Modifier.height(40.dp)
                ) {
                    Text(
                        when (selectedType) {
                            DanmakuType.SCROLL -> "滚动"
                            DanmakuType.TOP -> "顶部"
                            DanmakuType.BOTTOM -> "底部"
                        },
                        fontSize = 12.sp
                    )
                }

                // 输入框
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    placeholder = {
                        Text(
                            "发送友善的弹幕见证奇迹的时刻~",
                            color = Color.Gray,
                            fontSize = 14.sp
                        )
                    },
                    modifier = Modifier.weight(1f),
                    colors = OutlinedTextFieldDefaults.colors(
                        unfocusedTextColor = if (isDarkMode) Color.White else Color.Black,
                        focusedTextColor = if (isDarkMode) Color.White else Color.Black,
                        unfocusedBorderColor = Color.Gray.copy(alpha = 0.5f),
                        focusedBorderColor = Color(0xFFFB7299),
                        unfocusedContainerColor = Color.Transparent,
                        focusedContainerColor = Color.Transparent
                    ),
                    shape = RoundedCornerShape(20.dp),
                    singleLine = true
                )

                // 发送按钮
                Button(
                    onClick = {
                        if (text.isNotBlank()) {
                            val danmaku = Danmaku(
                                text = text.trim(),
                                time = currentTime,
                                type = selectedType,
                                color = selectedColor,
                                textSize = 32f
                            )
                            onSendDanmaku(danmaku)
                            text = ""
                        }
                    },
                    enabled = text.isNotBlank(),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Color(0xFFFB7299),
                        contentColor = Color.White,
                        disabledContainerColor = Color.Gray.copy(alpha = 0.5f)
                    ),
                    shape = RoundedCornerShape(20.dp),
                    modifier = Modifier.height(40.dp)
                ) {
                    Text("发送", fontSize = 14.sp, fontWeight = FontWeight.Bold)
                }
            }

            // 颜色选择器
            AnimatedVisibility(
                visible = showColorPicker,
                enter = slideInVertically() + fadeIn(),
                exit = slideOutVertically() + fadeOut()
            ) {
                Card(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 8.dp),
                    colors = CardDefaults.cardColors(
                        containerColor = if (isDarkMode) Color(0xFF2A2A2A) else Color.White
                    ),
                    elevation = CardDefaults.cardElevation(4.dp)
                ) {
                    LazyRow(
                        modifier = Modifier.padding(12.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        items(colorOptions) { color ->
                            Box(
                                modifier = Modifier
                                    .size(36.dp)
                                    .clip(CircleShape)
                                    .background(color)
                                    .border(
                                        width = if (color == selectedColor) 3.dp else 1.dp,
                                        color = if (color == selectedColor) Color(0xFFFB7299) else Color.Gray,
                                        shape = CircleShape
                                    )
                                    .clickable {
                                        selectedColor = color
                                        showColorPicker = false
                                    },
                                contentAlignment = Alignment.Center
                            ) {
                                if (color == selectedColor) {
                                    Icon(
                                        Icons.Default.Check,
                                        contentDescription = "已选择",
                                        tint = if (color == Color.White || color == Color.Yellow) Color.Black else Color.White,
                                        modifier = Modifier.size(16.dp)
                                    )
                                }
                            }
                        }
                    }
                }
            }

            // 弹幕类型选择器
            AnimatedVisibility(
                visible = showTypeSelector,
                enter = slideInVertically() + fadeIn(),
                exit = slideOutVertically() + fadeOut()
            ) {
                Card(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 8.dp),
                    colors = CardDefaults.cardColors(
                        containerColor = if (isDarkMode) Color(0xFF2A2A2A) else Color.White
                    ),
                    elevation = CardDefaults.cardElevation(4.dp)
                ) {
                    Column(
                        modifier = Modifier.padding(12.dp)
                    ) {
                        DanmakuType.values().forEach { type ->
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clickable {
                                        selectedType = type
                                        showTypeSelector = false
                                    }
                                    .padding(vertical = 8.dp, horizontal = 4.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                RadioButton(
                                    selected = type == selectedType,
                                    onClick = {
                                        selectedType = type
                                        showTypeSelector = false
                                    },
                                    colors = RadioButtonDefaults.colors(
                                        selectedColor = Color(0xFFFB7299)
                                    )
                                )
                                Spacer(modifier = Modifier.width(12.dp))
                                Column {
                                    Text(
                                        text = when (type) {
                                            DanmakuType.SCROLL -> "滚动弹幕"
                                            DanmakuType.TOP -> "顶部固定"
                                            DanmakuType.BOTTOM -> "底部固定"
                                        },
                                        color = if (isDarkMode) Color.White else Color.Black,
                                        fontWeight = FontWeight.Medium
                                    )
                                    Text(
                                        text = when (type) {
                                            DanmakuType.SCROLL -> "从右向左滚动显示"
                                            DanmakuType.TOP -> "在屏幕顶部固定显示"
                                            DanmakuType.BOTTOM -> "在屏幕底部固定显示"
                                        },
                                        color = Color.Gray,
                                        fontSize = 12.sp
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
package com.cicada.player.compose

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cicada.player.compose.ui.theme.CicadaPlayerTheme
import com.cicada.player.compose.upload.ui.UploadScreen

/**
 * 页面路由（官方 Navigation Compose 的字符串路由）。
 * 需要类型安全路由（`@Serializable` 数据类）时再说，那要额外加 kotlinx-serialization 插件。
 */
private object Routes {
    const val HOME = "home"
    const val PLAYER = "player"
    const val UPLOAD = "upload"
}

/**
 * **单 Activity + 官方 Navigation Compose**（`androidx.navigation:navigation-compose`）。
 *
 * 官方推荐的页面跳转就是这个：一个 Activity + [NavHost]，每个页面是一个 composable 目的地，
 * 跳转 `navController.navigate(...)`，返回 `popBackStack()`（多 Activity 是旧做法）。
 * 播放页的横竖屏/沉浸式系统栏在离开页面时由它自己恢复，见 VideoPlayerScreen 的 DisposableEffect。
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            CicadaPlayerTheme {
                val navController = rememberNavController()

                NavHost(navController = navController, startDestination = Routes.HOME) {
                    composable(Routes.HOME) {
                        HomeScreen(
                            onOpenPlayer = { navController.navigate(Routes.PLAYER) },
                            onOpenUpload = { navController.navigate(Routes.UPLOAD) },
                        )
                    }

                    composable(Routes.PLAYER) {
                        VideoPlayerScreen(onClose = { navController.popBackStack() })
                    }

                    composable(Routes.UPLOAD) {
                        UploadScreen(onClose = { navController.popBackStack() })
                    }
                }
            }
        }
    }
}

/** 首页：播放器与投稿页入口。 */
@Composable
private fun HomeScreen(onOpenPlayer: () -> Unit, onOpenUpload: () -> Unit) {
    Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Text(text = "Hello cicada!", fontSize = 28.sp)

            Spacer(modifier = Modifier.height(40.dp))

            Button(
                onClick = onOpenPlayer,
                shape = RoundedCornerShape(22.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFFB7299)),
                modifier = Modifier.padding(8.dp),
            ) {
                Text("打开视频播放器")
            }

            Button(
                onClick = onOpenUpload,
                shape = RoundedCornerShape(22.dp),
                colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFFB7299)),
                modifier = Modifier.padding(8.dp),
            ) {
                Text("视频投稿")
            }
        }
    }
}

package com.cicada.player.compose.components.video

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.cicada.player.compose.player.CicadaPlayerController

/**
 * 播放器 ViewModel：持有一个内核控制器（`CicadaPlayerController` → `:cicadaplayer` 内核）。
 * **不使用 ExoPlayer/Media3**（旧版这里是 ExoPlayer 的 ViewModel，已替换）。
 */
class PlayerViewModel(application: Application) : AndroidViewModel(application) {

    val controller = CicadaPlayerController(application)

    /** 地址或播放对象（`{` 开头按 MediaManifest 播放对象处理） */
    fun loadMedia(source: String) = controller.playAny(source)

    fun togglePlayback() = controller.togglePlay()

    override fun onCleared() {
        super.onCleared()
        controller.release()
    }
}

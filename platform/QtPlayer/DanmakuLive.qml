// ===========================================================================
// DanmakuLive —— 实时弹幕的 WebSocket 客户端（独立文件，**故意**不放在 DanmakuView.qml 里）
//
// 【为什么必须单独一个文件】`import QtWebSockets` 是**硬依赖**：某个 kit 里没有这个 QML 模块时，
// 带着这句 import 的文件会**整个加载失败** —— DanmakuView 挂了 → PlayerView 跟着挂 →
// 播放器窗口根本建不出来（用户实测："点击视频打不开窗口了"）。
// 放在独立文件里、由 DanmakuView 用 **Loader 动态加载**，模块缺失时只是这个 Loader 加载失败、
// 打一条日志，实时弹幕降级为不可用，**窗口和其余弹幕功能完全不受影响**。
//
// 契约（mock 那边定的，见 front/player/mock-server/README.md）：
//   地址  ws://<mock>/danmaku/ws
//   下行  {"type":"danmaku","text":…,"timeSec":12.34,"mode":"scroll|top|bottom",
//          "fontSize":25,"color":"#FFFFFF","uid":"1","id":"dm-7","seq":7}
//         连上先收一条 hello；{"type":"pong"} 是 ping 的回应
//   上行  DanmakuView 走 POST /danmaku/send（本文件只负责收）
//
// 【为什么是 WebSocket 而不是轮询】轮询是"每 N 毫秒发一次请求"，没有新弹幕时也在空转；
// WS 是服务端推，只有真有新弹幕时才来数据。历史弹幕走 /x/v1/dm/list.so，不由这里管。
// ===========================================================================
import QtQuick
import QtWebSockets

Item {
    id: live

    /* mock 的 WS 地址；空串 = 不连接（DanmakuView 探测到基址后写进来） */
    property string liveUrl: ""

    /* 收到一条新弹幕（已 JSON.parse 成对象）→ 交给 DanmakuView 上屏 */
    signal danmakuReceived(var message)

    /* 出问题时给外面一条日志（不弹框、不中断） */
    signal failed(string reason)

    visible: false

    /* 断线重连：QML 的 WebSocket 不会自己重连，这里 3 秒试一次 */
    Timer {
        interval: 3000
        repeat: true
        running: live.liveUrl !== ""
        onTriggered: if (socket.status !== WebSocket.Open
                          && socket.status !== WebSocket.Connecting)
                         socket.active = true
    }

    WebSocket {
        id: socket

        url: live.liveUrl
        active: live.liveUrl !== ""

        onTextMessageReceived: function (text) {
            var msg = null

            try {
                msg = JSON.parse(text)
            } catch (e) {
                live.failed("收到非 JSON：" + text)
                return
            }

            if (msg !== null && msg.type !== undefined)
                live.danmakuReceived(msg)
        }
        onStatusChanged: if (status === WebSocket.Error)
                             live.failed("连接出错：" + errorString)
    }
}

// ===========================================================================
// 交互卡片模式：展示（interactive）/ 编辑（edit）的全局开关
//
// 为什么单独一个文件、还要 `pragma Singleton`（和 QtPlayerPreviewMode 同一条路）：
//   * 这个状态是**应用级**的：首页（HomeWindow.qml）的切换按钮写它，
//     播放器窗口（Main.qml）的交互层和右侧 API 面板按它决定行为 ——
//     两个窗口是两份 QML 树，摸不到彼此的属性，只有单例是两边共享的同一份；
//   * 播放器窗口开着的时候切模式，单例的属性变化信号会推过去，
//     InteractionLayer 据此进入/退出编辑态（卡片常驻显示、可拖拽、带参考线），
//     不用重开窗口、也不用重拉数据。
//
// 两个值的含义（照 hili-player demo 里 makeInteractionConfig 的 mode）：
//   * "interactive" —— 展示模式（默认，加这个开关之前的行为）：卡片按
//                     时间窗出现/消失，观众只能投票/评分/点链接；
//   * "edit"        —— 编辑模式：卡片常驻（不看时间窗）、可拖拽改位置、
//                     右侧面板可增删改卡片与选项。
//
// 注意：本文件必须在 CMake 里登记成单例（QT_QML_SINGLETON_TYPE TRUE），
// 和 QtPlayerPreviewMode.qml 的登记方式一模一样，少了那一句会报 "is not a type"。
// ===========================================================================
pragma Singleton
import QtQuick

QtObject {
    /* 交互卡片模式："interactive"（展示）或 "edit"（编辑）。
       默认 "interactive"：和加这个开关之前的行为一致。 */
    property string interactionMode: "interactive"
}

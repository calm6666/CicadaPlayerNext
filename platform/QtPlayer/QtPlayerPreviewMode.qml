// ===========================================================================
// 预览图模式：雪碧图 / 逐帧 bin 的全局开关
//
// 为什么单独一个文件、还要 `pragma Singleton`（和 QtPlayerTheme 同一条路）：
//   * 这个状态是**应用级**的：首页（HomeWindow.qml）的切换按钮写它，
//     播放器窗口（Main.qml）按它决定拉 /videoshot/index.json（雪碧图）
//     还是 /videoshot/preview.bin（逐帧 data URL）—— 两个窗口是两份 QML 树，
//     摸不到彼此的属性，只有单例是两边共享的同一份；
//   * 播放器窗口开着的时候切模式，单例的属性变化信号会推过去，
//     Main.qml 里据此重拉预览数据（不用重开窗口、也不用重拉弹幕/分段那些）。
//
// 两个值的含义：
//   * "bin"    —— 逐帧模式（默认，加这个开关之前的行为）：preview.bin 按
//                 \u001F 切成 data URL 数组，气泡里 arr[floor(秒/5)+1] 取帧；
//   * "sprite" —— 雪碧图模式（B 站形状）：index.json 的 pvdata 给出
//                 img_url + img_x_len/img_y_len/img_x_size/img_y_size，
//                 下标 = floor(秒/5)，col = 下标 % xLen、row = floor(下标/xLen)，
//                 用 Image.sourceClipRect 从大图上裁出那一格。
//
// 注意：本文件必须在 CMake 里登记成单例（QT_QML_SINGLETON_TYPE TRUE），
// 和 QtPlayerTheme.qml 的登记方式一模一样，少了那一句会报 "is not a type"。
// ===========================================================================
pragma Singleton
import QtQuick

QtObject {
    /* 预览图模式："bin"（逐帧 preview.bin）或 "sprite"（雪碧图裁切）。
       默认 "bin"：和加这个开关之前的行为一致。 */
    property string mode: "bin"
}

# macOS / Linux 打包流程

## 1. macOS

```bash
cd external
./build_external.sh macOS      # native 编译，MACOS_ARCHS="x86_64 arm64"
```

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8 cicadaPlayer
```

产物：`cmdline/cicadaPlayer`（SDL 渲染的命令行播放器）与 `libmedia_player.a`（供接入方使用）。

## 2. Linux

```bash
cd external
./build_external.sh Linux      # native 编译
```

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DUSEASAN=OFF ..
make -j8 cicadaPlayer
```

依赖（Ubuntu 22.04）：`build-essential cmake yasm nasm libssl-dev libcurl4-openssl-dev libsdl2-dev`。
`framework/Linux.cmake` 负责 include/lib 路径与 FRAMEWORK_LIBS。

## 3. Windows（MinGW 交叉编译）

```bash
# 依赖见 build_tools/README.md “MinGW 编译Windows环境说明”
cd external
./build_external.sh Windows    # WIN32_ARCHS="i686 x86_64"
```

```bash
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmdline/toolchain.windows.cmake ..
make -j8 cicadaPlayer
```

输出 `cicadaPlayer.exe` + `libalivcffmpeg.dll`（`install/ffmpeg/win32/<arch>/`）。
本机 MSVC 构建见 `doc/compile_Windows_msvc.md`。

## 4. 通用发布注意

- 各平台均基于 `build_tools/ffmpeg_cross_compile_config.sh` 的 FFmpeg 9.0 配置；
- 动态库打包策略见 `build_tools/README.md` 第 3 节（所有静态库合并进 libalivcffmpeg）。

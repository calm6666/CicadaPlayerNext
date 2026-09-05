# iOS 打包流程

## 1. 环境要求

- macOS + Xcode 14+（FFmpeg 9.0 需要较新 clang）
- CMake 3.15+、yasm/nasm、meson+ninja（dav1d 可选）

## 2. 编译外部库

```bash
cd external
./build_external.sh iOS
```

流程：`build_iOS.sh` → 对 `IOS_ARCHS="arm64 x86_64"` 逐个
`build_static_lib iOS <arch>`，最终 FFmpeg 与依赖被合并进
`install/ffmpeg/iOS/<arch>/` 下的动态 framework（详见 `build_tools/build_iOS.sh`）。

## 3. 编译播放器 SDK / Demo

```bash
cd mediaPlayer
# 通过 Xcode 工程或 CMake 构建（framework/iOS.cmake 提供路径与库）
cmake -G Xcode -DIOS=ON ..
```

SDK 产物：`iOS` 下的动态 framework（含 `libalivcffmpeg` + 播放器代码）。
Demo：参考 `doc/compile_ios.md` 中的 Xcode 集成步骤。

## 4. 发布注意事项

- 真机 `arm64` + 模拟器 `x86_64` 双架构；上架需 bitcode/签名由 Xcode 完成；
- 硬解走 VideoToolbox（`ENABLE_VTB_DECODER`）；DRM（FairPlay）通过 `keyFormat` 通道。

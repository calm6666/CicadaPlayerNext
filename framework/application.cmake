# Legacy standalone Android toolchain (kept for framework/demuxer/build_android.sh).
# Modern NDK r25+ only: clang toolchain, c++_static runtime, API 24 minimum
# (Android 7.0 — matches the player's new minimum supported version).
set(ANDROID_TOOLCHAIN clang)
set(ANDROID_PLATFORM android-24)
set(ANDROID_STL c++_static)

include($ENV{NDK_ROOT}/build/cmake/android.toolchain.cmake)

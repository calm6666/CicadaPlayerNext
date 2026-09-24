# ===========================================================================
# Danmaku —— 独立弹幕引擎（纯 C++11，标准库，无 Qt / 无框架 / 无第三方）
#
# 作为独立工程构建：
#     cmake -S . -B build
#     cmake --build build --config Release
#     cmake -S . -B build -DDANMAKU_BUILD_TESTS=ON
#     ctest --test-dir build -C Release       # 或者直接跑 danmaku_selftest
#
# 作为子工程嵌入（不会污染父工程）：
#     add_subdirectory(third_party/danmaku)
#     target_link_libraries(your_target PRIVATE Danmaku)
# ===========================================================================
cmake_minimum_required(VERSION 3.10)

project(Danmaku
        VERSION 1.0.0
        DESCRIPTION "Standalone cross-platform danmaku (bullet comment) engine, pure C++11"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
option(DANMAKU_BUILD_SHARED "构建动态库 Danmaku（OFF = 静态库）" ON)
option(DANMAKU_BUILD_TESTS  "构建自测程序 danmaku_selftest"          OFF)

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
set(DANMAKU_SOURCES
    src/DanmakuTypes.cpp
    src/DanmakuEngine.cpp
    src/DanmakuXml.cpp)

set(DANMAKU_HEADERS
    include/Danmaku/DanmakuExport.h
    include/Danmaku/DanmakuTypes.h
    include/Danmaku/DanmakuEngine.h
    include/Danmaku/DanmakuXml.h
    include/Danmaku/Danmaku.h)

if(DANMAKU_BUILD_SHARED)
    add_library(Danmaku SHARED ${DANMAKU_SOURCES} ${DANMAKU_HEADERS})
    # 编译本库自己时定义：Windows 上导出 __declspec(dllexport)
    target_compile_definitions(Danmaku PRIVATE DANMAKU_BUILDING)
else()
    add_library(Danmaku STATIC ${DANMAKU_SOURCES} ${DANMAKU_HEADERS})
    # 静态库：告诉使用方"别走 dllimport"，随目标透出
    target_compile_definitions(Danmaku PUBLIC DANMAKU_STATIC)
    # 静态库也能被链进别的动态库
    set_target_properties(Danmaku PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(Danmaku
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# 公开头文件只用 C++11 语法；把最低标准透出给使用方
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.8)
    target_compile_features(Danmaku PUBLIC cxx_std_11)
endif()

set_target_properties(Danmaku PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# 源码是 UTF-8（无 BOM）：MSVC 默认按 ANSI(cp936) 解码会把中文注释和紧随其后的
# 代码行搅在一起（与 CicadaPlayerNext/platform/QtPlayer/CMakeLists.txt:161-167 同样的处理）。
# C4251 是"导出类含 STL 成员"的噪音警告，不是错误，关掉。
if(MSVC)
    target_compile_options(Danmaku PRIVATE /utf-8 /wd4251)
endif()

# ---------------------------------------------------------------------------
# 自测
# ---------------------------------------------------------------------------
if(DANMAKU_BUILD_TESTS)
    add_executable(danmaku_selftest tests/selftest.cpp)
    target_link_libraries(danmaku_selftest PRIVATE Danmaku)
    set_target_properties(danmaku_selftest PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(danmaku_selftest PRIVATE /utf-8 /wd4251)
    endif()

    # 动态库时把 DLL 拷到可执行文件旁边，直接跑就行
    if(WIN32 AND DANMAKU_BUILD_SHARED)
        add_custom_command(TARGET danmaku_selftest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:Danmaku>
                    $<TARGET_FILE_DIR:danmaku_selftest>
            VERBATIM)
    endif()

    # 注册 ctest 用例：独立构建时 ctest 直接可用；作为子工程时也能被父工程的 ctest 找到
    enable_testing()
    add_test(NAME danmaku_selftest COMMAND danmaku_selftest)
endif()

# ---------------------------------------------------------------------------
# 安装（只在独立构建时挂 install 规则，add_subdirectory 时不污染父工程）
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(GNUInstallDirs)

    install(TARGETS Danmaku
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/Danmaku
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

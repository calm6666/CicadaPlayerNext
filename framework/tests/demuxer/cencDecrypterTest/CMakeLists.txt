cmake_minimum_required(VERSION 3.6)
project(cencDecrypterTest LANGUAGES CXX)

# require C++11 (工程其它测试同款设置；本测试只用 C++11 特性)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    include(../Apple.cmake)
endif ()

include(../../${TARGET_PLATFORM}.cmake)

# 这个测试不需要网络、不需要 gtest_main 之外的任何测试夹具；
# 依赖 demuxer 是为了 CENCDecrypter 与 avAESDecrypt/avAESEncrypt 的实现符号。
add_executable(cencDecrypterTest "")

target_sources(cencDecrypterTest
        PRIVATE
        cencDecrypterTest.cpp
        )

target_include_directories(
        cencDecrypterTest
        PRIVATE
        ../../
        ${COMMON_INC_DIR}
)

# 链接列表与 demuxerUnitTest / dataSourceTest 保持一致（demuxer 是静态库，
# 这里要把它需要的第三方库补齐）
target_link_libraries(
        cencDecrypterTest PRIVATE
        demuxer
        framework_utils
        framework_drm
        data_source
        avformat
        avcodec
        swresample
        avutil
        swscale
        xml2
        z
        curl
        gtest_main)

target_link_directories(cencDecrypterTest PRIVATE ${COMMON_LIB_DIR})

if (APPLE)
    target_link_libraries(
            cencDecrypterTest PUBLIC
            iconv
            bz2
            ${FRAMEWORK_LIBS}
    )
else ()
    target_link_libraries(
            cencDecrypterTest PUBLIC
            dl
            ssl
            crypto
            pthread
    )
endif ()

if (HAVE_COVERAGE_CONFIG)
    target_link_libraries(cencDecrypterTest PUBLIC coverage_config)
endif ()

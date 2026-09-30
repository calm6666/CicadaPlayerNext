cmake_minimum_required(VERSION 3.6)

project(external)
include(config.cmake)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
if (IOS)
    set(IPHONEOS_DEPLOYMENT_TARGET 8.0)
    set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET ${IPHONEOS_DEPLOYMENT_TARGET})
else ()
    # 10.11 早已低于实际要求：CVMetalTextureCache 要 10.11、VideoToolbox 的 HEVC 探测
    # 用 __builtin_available(macOS 10.13)、Qt 6.x 自身要求更高，而 arm64 的 Mac 起步就是
    # 11.0。旧值会让 Xcode 每次都打 "deployment target 10.11 ... supported ... 10.13 to
    # 15.x" 的警告，并让链接期出现"object 为新系统编的、却按 11.0 链接"的噪音。
    set(MACOSX_DEPLOYMENT_TARGET 11.0)
    set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET ${MACOSX_DEPLOYMENT_TARGET})
endif ()

set(LINK_OPTS -all_load)
if ("${IOS_PLATFORM}" STREQUAL "SIMULATOR64")
    set(LINK_OPTS "${LINK_OPTS},-read_only_relocs,suppress")
    #else()
    #    set(LINK_OPTS "${LINK_OPTS},-fembed-bitcode")
endif ()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fembed-bitcode")
set(LIBRARYS ${SRC_LIBRARIES})

link_directories(${SRC_LIBRARIES_DIR})
include_directories(${SRC_INCLUDE_DIR})
add_library(${LIB_NAME} SHARED
        build_version.cpp
        )
if (IOS)
set_target_properties(${LIB_NAME} PROPERTIES
        FRAMEWORK TRUE
        FRAMEWORK_VERSION A
        MACOSX_FRAMEWORK_IDENTIFIER com.alibaba.AliyunPlayer.${LIB_NAME}
        LINK_FLAGS "-Wl,${LINK_OPTS}"
        LINKER_LANGUAGE C
        MACOSX_FRAMEWORK_BUNDLE_VERSION 1
        MACOSX_FRAMEWORK_SHORT_VERSION_STRING 1.0
        INSTALL_NAME_DIR @rpath
        XCODE_ATTRIBUTE_LD_DYLIB_INSTALL_NAME "@rpath/${LIB_NAME}.framework/${LIB_NAME}"
        #        MACOSX_FRAMEWORK_INFO_PLIST Info.plist
        #        PUBLIC_HEADER dynamicFramework.h
        # XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer"
        XCODE_ATTRIBUTE_EXCLUDED_ARCHS[sdk=iphonesimulator*] arm64
        XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS YES
        XCODE_ATTRIBUTE_ENABLE_BITCODE "YES"
        XCODE_ATTRIBUTE_BITCODE_GENERATION_MODE bitcode
        )
else ()
    set_target_properties(${LIB_NAME} PROPERTIES
            FRAMEWORK TRUE
            FRAMEWORK_VERSION A
            MACOSX_FRAMEWORK_IDENTIFIER com.alibaba.AliyunPlayer.${LIB_NAME}
            LINK_FLAGS "-Wl,${LINK_OPTS}"
            LINKER_LANGUAGE C
            MACOSX_FRAMEWORK_BUNDLE_VERSION 1
            MACOSX_FRAMEWORK_SHORT_VERSION_STRING 1.0
            INSTALL_NAME_DIR @rpath
            XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET ${MACOSX_DEPLOYMENT_TARGET}
            XCODE_ATTRIBUTE_LD_DYLIB_INSTALL_NAME "@rpath/${LIB_NAME}.framework/${LIB_NAME}"
            )
endif ()
#if (IOS)
#    macro(set_xcode_property TARGET XCODE_PROPERTY XCODE_VALUE)
#        set_property(TARGET ${TARGET} PROPERTY XCODE_ATTRIBUTE_${XCODE_PROPERTY}
#                ${XCODE_VALUE})
#    endmacro(set_xcode_property)
#endif ()
#
#set_xcode_property(${LIB_NAME} GCC_GENERATE_DEBUGGING_SYMBOLS[variant=Release] YES)
#if (IOS)
#    set_xcode_property(${LIB_NAME} ENABLE_BITCODE "YES")
#    set_xcode_property(${LIB_NAME} BITCODE_GENERATION_MODE bitcode)
#endif ()
target_link_libraries(${LIB_NAME} PRIVATE ${LIBRARYS})
target_link_libraries(${LIB_NAME} PUBLIC "${LINK_FRAMEWORKS}")

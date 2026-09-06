#!/bin/bash

source utils.sh
function ffmpeg_config_reset(){
    FFMPEG_DECODER_LIST_ADDED=""
    FFMPEG_ENCODER_LIST_ADDED=""
    FFMPEG_MUXER_LIST_ADDED=""
    FFMPEG_PARSER_LIST_ADDED=""
    FFMPEG_HWACCEL_LIST_ADDED=""
    FFMPEG_PROTOCOL_LIST_ADDED=""
    FFMPEG_BSF_LIST_ADDED=""
    FFMPEG_FILTER_LIST_ADDED=""
    FFMPEG_DEMUXER_LIST_ADDED=""

    ffmpeg_config_user=""

    ffmpeg_decoder_config=""
    ffmpeg_encoder_config=""
    ffmpeg_demuxer_config=""
    ffmpeg_parser_config=""
    ffmpeg_hwaccel_config=""
    ffmpeg_protocol_config=""
    ffmpeg_bsf_config=""
    ffmpeg_filter_config=""
    ffmpeg_cross_compile_config=""
    ffmpeg_extra_cflags=""
    ffmpeg_extra_ldlags=""
    ffmpeg_config_cc=""
    ffmpeg_install_dir=""
}
component_classes="decoder encoder demuxer muxer parser hwaccel protocol bsf filter";

ffmpeg_disable_all_config="--disable-everything \
  --disable-programs\
  --disable-doc \
  --disable-filters \
  --disable-avdevice \
  --disable-hwaccels \
  --disable-bzlib    \
  --disable-videotoolbox"
# libavresample was removed in FFmpeg 5.0; libpostproc was removed in FFmpeg 7.0.
# The resampling library is libswresample.
# --disable-debug：发布构建不带 -g/assert（FFmpeg 默认 debug=yes，体积翻 2~3 倍）；
# BUILD_TYPE=Debug 时后面会追加 --enable-debug=3 覆盖。
ffmpeg_config_default="--enable-pic --disable-debug"

ffmpeg_config_debug="--disable-optimizations \
  --disable-asm \
  --disable-stripping \
  --enable-debug=3"

ffmpeg_inited=false

function ffmpeg_init_vars(){
    if [[ "${ffmpeg_inited}" == "TRUE" ]]
    then
        echo "inited"
        return
    fi

    if [[ -f ${FFMPEG_SOURCE_DIR}/config.h ]]
    then
        rm ${FFMPEG_SOURCE_DIR}/config.h
    fi
    FFMPEG_DECODER_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure --list-decoders`
    FFMPEG_ENCODER_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure --list-encoders`
    FFMPEG_DEMUXER_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure --list-demuxers`
    FFMPEG_MUXER_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure   --list-muxers`
    FFMPEG_PARSER_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure  --list-parsers`
    FFMPEG_HWACCEL_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure --list-hwaccels`
    FFMPEG_PROTOCOL_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure --list-protocols`
    FFMPEG_BSF_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure --list-bsfs`
    FFMPEG_FILTER_LIST_SUPPORT=`${FFMPEG_SOURCE_DIR}/configure --list-filters`
    ffmpeg_inited=TRUE
}

function ffmpeg_config_add_decoders(){
    local i;
    for i in $*; do
        ffmpeg_config_add_component decoder ${i}
        local ret=$?
        if [[ ${ret} -eq 0 ]]
        then
            FFMPEG_DECODER_LIST_ADDED="${FFMPEG_DECODER_LIST_ADDED} ${i}"
        elif [[ ${ret} -eq 2 ]]
        then
            exit 1
        fi
    done
    return 0;
}
function ffmpeg_config_add_encoders(){
    local i;
    for i in $*; do
        ffmpeg_config_add_component encoder ${i}
        if [[ "$?" == "0" ]]
        then
            FFMPEG_ENCODER_LIST_ADDED="${FFMPEG_ENCODER_LIST_ADDED} $i"
        fi
    done
    return 0;
}

function ffmpeg_config_add_demuxers(){
    local i;
    echo $#
    for i in $*; do
#        echo to add demuxer ${i}
        ffmpeg_config_add_component demuxer  ${i}
        local ret=$?
        if [[ ${ret} -eq 0 ]]
        then
#            echo add demuxer ${i}
            FFMPEG_DEMUXER_LIST_ADDED="${FFMPEG_DEMUXER_LIST_ADDED}  ${i}"
        fi
    done
    return 0;
}

function ffmpeg_config_add_muxers(){
    local i;
    for i in $*; do
        ffmpeg_config_add_component muxer  ${i}
        if [[ $? == 0 ]]
        then
            FFMPEG_MUXER_LIST_ADDED="${FFMPEG_MUXER_LIST_ADDED}  ${i}"
        fi
    done
    return 0;
}

function ffmpeg_config_add_parsers(){
    local i;
    for i in $*; do
        ffmpeg_config_add_component parser  ${i}
        if [[ $? == 0 ]]
        then
            FFMPEG_PARSER_LIST_ADDED="${FFMPEG_PARSER_LIST_ADDED}  ${i}"
        fi
    done
    return 0;
}

function ffmpeg_config_add_bsfs(){
    local i;
    for i in $*; do
        ffmpeg_config_add_component bsf  ${i}
        if [[ $? == 0 ]]
        then
            FFMPEG_BSF_LIST_ADDED="${FFMPEG_BSF_LIST_ADDED} ${i}"
        fi
    done
    return 0;
}

function ffmpeg_config_add_hwaccels(){
    local i;
    for i in $*; do
        ffmpeg_config_add_component hwaccel ${i}
        if [[ $? == 0 ]]
        then
            FFMPEG_HWACCEL_LIST_ADDED="${FFMPEG_HWACCEL_LIST_ADDED} ${i}"
        fi
    done
    return 0;
}

function ffmpeg_config_add_protocols(){
    local i;
    for i in $*; do
 #       echo to add protocol ${i}
        ffmpeg_config_add_component protocol ${i}
        if [[ $? == 0 ]]
        then
#            echo  add protocol ${i}
            FFMPEG_PROTOCOL_LIST_ADDED="${FFMPEG_PROTOCOL_LIST_ADDED} ${i}"
        fi
    done
    return 0;
}

function ffmpeg_config_add_filters(){
    local i;
    for i in $*; do
        ffmpeg_config_add_component filter ${i}
        if [[ $? == 0 ]]
        then
            FFMPEG_FILTER_LIST_ADDED="${FFMPEG_FILTER_LIST_ADDED} ${i}"
        fi
    done
    return 0;
}

function ffmpeg_config_add_component(){
    local component_list="";
    local component_class;
    ffmpeg_check_component_supported $1 $2
    if [[ $? -ne 0 ]]
    then
        print_error "$1 $2 not support"
        return 2
    fi
    for component_class in ${component_classes}
    do
        if [[ "$1" == "${component_class}" ]]
        then
            local component=$(echo ${component_class} | tr '[a-z]' '[A-Z]')
            component_list=FFMPEG_${component}_LIST_ADDED
        fi
    done

    local component
    for component in ${!component_list}
    do
        if [[ "$component" == "$2" ]]
        then
            print_warning "$1 $2 already added"
            return 1;
        fi
    done
#   ${!component_list}="${!component_list} $1"

    return 0;

}

#function ffmpeg_check_decoder_supported(){
#    ffmpeg_check_component_supported decoder $1
#}
#
#function ffmpeg_check_encoder_supported(){
#    ffmpeg_check_component_supported encoder $1
#}
#
#function ffmpeg_check_demuxer_supported(){
#    ffmpeg_check_component_supported demuxer $1
#}
#function ffmpeg_check_muxer_supported(){
#    ffmpeg_check_component_supported muxer $1
#}
#function ffmpeg_check_parser_supported(){
#    ffmpeg_check_component_supported parser $1
#}
#function ffmpeg_check_hwaccel_supported(){
#    ffmpeg_check_component_supported hwaccel $1
#}
#function ffmpeg_check_protocol_supported(){
#    ffmpeg_check_component_supported protocol $1
#}

function ffmpeg_check_component_supported(){
    local component_list="";
    for component_class in ${component_classes}
    do
        if [[ "$1" == "${component_class}" ]]
        then
            local component=$(echo ${component_class} | tr '[a-z]' '[A-Z]')
            component_list=FFMPEG_${component}_LIST_SUPPORT
        fi
    done

    if [[ -z "${!component_list}" ]]
    then
        echo component_class $1 not found
        return -1;
    fi
    local component;

    for component in ${!component_list}
    do
        # FFmpeg 的组件名经常是逗号分隔的别名（如 demuxer
        # "mov,mp4,m4a,3gp,3g2,mj2"、demuxer "hls,applehttp"），
        # --enable-xxx=别名 是合法的，这里按逗号分段匹配。
        if [[ ",${component}," == *",$2,"* ]]
        then
#            echo support $1 $2
            return 0
        fi
    done
    echo not support $1 $2
    return -1;
}

function ffmpeg_generate_decoder_config(){
    local decoder;
    for decoder in ${FFMPEG_DECODER_LIST_ADDED}
    do
        ffmpeg_decoder_config="${ffmpeg_decoder_config} --enable-decoder=${decoder}"
    done
#    echo ${ffmpeg_decoder_config}
}

function ffmpeg_generate_encoder_config(){
    local encoder;
    for encoder in ${FFMPEG_ENCODER_LIST_ADDED}
    do
        ffmpeg_encoder_config="${ffmpeg_encoder_config} --enable-encoder=${encoder}"
    done
#    echo ${ffmpeg_encoder_config}
}

function ffmpeg_generate_demuxer_config(){
    local demuxer;
    for demuxer in ${FFMPEG_DEMUXER_LIST_ADDED}
    do
        ffmpeg_demuxer_config="${ffmpeg_demuxer_config} --enable-demuxer=${demuxer}"
    done
    echo ${ffmpeg_demuxer_config}
}

function ffmpeg_generate_muxer_config(){
    local muxer;
    for muxer in ${FFMPEG_MUXER_LIST_ADDED}
    do
        ffmpeg_muxer_config="${ffmpeg_muxer_config} --enable-muxer=${muxer}"
    done
#    echo ${ffmpeg_muxer_config}
}

function ffmpeg_generate_parser_config(){
    local parser;
    for parser in ${FFMPEG_PARSER_LIST_ADDED}
    do
        ffmpeg_parser_config="${ffmpeg_parser_config} --enable-parser=${parser}"
    done
#    echo ${ffmpeg_parser_config}
}

function ffmpeg_generate_hwaccel_config(){
    local hwaccel;
    for hwaccel in ${FFMPEG_HWACCEL_LIST_ADDED}
    do
        ffmpeg_hwaccel_config="${ffmpeg_hwaccel_config} --enable-hwaccel=${hwaccel}"
    done
#    echo ${ffmpeg_hwaccel_config}
}

function ffmpeg_generate_protocol_config(){
    local protocol;
    for protocol in ${FFMPEG_PROTOCOL_LIST_ADDED}
    do
        ffmpeg_protocol_config="${ffmpeg_protocol_config} --enable-protocol=${protocol}"
    done
    echo ${ffmpeg_protocol_config}
}

function ffmpeg_generate_filter_config(){
    local filter;
    for filter in ${FFMPEG_FILTER_LIST_ADDED}
    do
        ffmpeg_filter_config="${ffmpeg_filter_config} --enable-filter=${filter}"
    done
#    echo ${ffmpeg_protocol_config}
}

function ffmpeg_generate_bsf_config(){
    local bsf;
    for bsf in ${FFMPEG_BSF_LIST_ADDED}
    do
        ffmpeg_bsf_config="${ffmpeg_bsf_config} --enable-bsf=${bsf}"
    done
#    echo ${ffmpeg_bsf_config}
}

function ffmpeg_config_add_user(){
    ffmpeg_config_user="${ffmpeg_config_user} $1"

}
function ffmpeg_config_set_cross_config(){
    ffmpeg_cross_compile_config="$1"
}
function ffmpeg_config_set_cc(){
    ffmpeg_config_cc="$1"
}
function ffmpeg_config_set_as(){
    ffmpeg_config_as="$1"
}

function ffmpeg_config_add_extra_cflags(){
    if [[ "$1" == "" ]]
    then
        return;
    fi
    if [[ "$ffmpeg_extra_cflags" == "" ]]
    then
        ffmpeg_extra_cflags="$1"
    else
        ffmpeg_extra_cflags="$ffmpeg_extra_cflags $1"
    fi

}
function ffmpeg_config_add_extra_ldflags(){
    if [[ "$1" == "" ]]
    then
        return;
    fi
    if [[ "$ffmpeg_extra_ldlags" == "" ]]
    then
        ffmpeg_extra_ldlags="$1"
    else
        ffmpeg_extra_ldlags="$ffmpeg_extra_ldlags $1"
    fi
}
function ffmpeg_config_set_install(){
    ffmpeg_install_dir=$1

}
# 配置核对：把清单里请求的组件与 configure 实际生成的组件宏逐一比对。
# FFmpeg 9.0 的组件宏（CONFIG_xxx_DECODER 等）在 config_components.h，
# config.h 只保留库级宏并 #include 它。
#
# 严格度分级：
#   - decoder / parser / bsf：禁用 = 致命（缺解码器/解析器直接导致无法播放）
#   - demuxer / muxer / protocol / hwaccel：禁用 = 仅警告。部分组件按设计
#     就不链入 FFmpeg：dash_demuxer（需要 libxml2，播放器用自带实现）、
#     https_protocol/tls（TLS 走 curl），configure 禁用它们属于预期行为。
function ffmpeg_verify_requested_components(){
    # FFmpeg 9.0：组件宏（CONFIG_xxx_DECODER / _DEMUXER / _BSF ...）写在
    # config_components.h 里，config.h 只保留库级宏并 #include 它。
    local comp_header="config_components.h"
    if [[ ! -f "${comp_header}" ]]; then
        echo "ERROR: ${comp_header} not found — configure did not produce output?"
        return 1
    fi

    local class class_upper listvar name base macro
    for class in decoder parser bsf; do
        listvar="FFMPEG_$(echo "${class}" | tr '[a-z]' '[A-Z]')_LIST_ADDED"
        for name in ${!listvar}; do
            [[ -z "${name}" ]] && continue
            base="${name%%,*}"
            macro="CONFIG_$(echo "${base}" | tr '[a-z]-' '[A-Z]_')_$(echo "${class}" | tr '[a-z]' '[A-Z]')"
            if ! grep -q "^#define ${macro} 1" "${comp_header}"; then
                echo "ERROR: requested ${class} '${name}' was disabled by configure (${macro} != 1 in ${comp_header})"
                if [[ -f ffbuild/config.log ]]; then
                    grep -iE "disabled .*${base}|${base}.*disabled" ffbuild/config.log | tail -5
                fi
                echo "       fix: enable the missing dependency shown above in player_ffmpeg_config.sh"
                echo "            (full reason: grep -i '${base}' ffbuild/config.log)"
                return 1
            fi
        done
    done
    for class in demuxer muxer protocol hwaccel; do
        listvar="FFMPEG_$(echo "${class}" | tr '[a-z]' '[A-Z]')_LIST_ADDED"
        for name in ${!listvar}; do
            [[ -z "${name}" ]] && continue
            base="${name%%,*}"
            macro="CONFIG_$(echo "${base}" | tr '[a-z]-' '[A-Z]_')_$(echo "${class}" | tr '[a-z]' '[A-Z]')"
            if ! grep -q "^#define ${macro} 1" "${comp_header}"; then
                echo "WARN: ${class} '${name}' disabled by configure (${macro} != 1 in ${comp_header}) — non-fatal"
            fi
        done
    done
    return 0
}

function ffmpeg_config(){
    ffmpeg_generate_decoder_config
    ffmpeg_generate_encoder_config
    ffmpeg_generate_demuxer_config
    ffmpeg_generate_muxer_config
    ffmpeg_generate_parser_config
    ffmpeg_generate_bsf_config
    ffmpeg_generate_protocol_config
    ffmpeg_generate_filter_config
    ffmpeg_generate_hwaccel_config

    local component_config="${ffmpeg_decoder_config} \
         ${ffmpeg_encoder_config} \
         ${ffmpeg_demuxer_config} \
         ${ffmpeg_muxer_config}   \
         ${ffmpeg_parser_config}  \
         ${ffmpeg_hwaccel_config} \
         ${ffmpeg_protocol_config} \
         ${ffmpeg_bsf_config}     \
         ${ffmpeg_filter_config}"

    local ff_config="${ffmpeg_disable_all_config}
        ${ffmpeg_config_default}
        ${ffmpeg_config_user}
        ${component_config}
        ${ffmpeg_cross_compile_config}"

    if [[ "${BUILD_TYPE}" == "Debug" ]];then
        ff_config="${ff_config} ${ffmpeg_config_debug}"
    fi

    echo ${ff_config}
    local configure_ret=0
    if [[ "${BUILD}" != "False" ]] || [[ "${BUILD_FFMPEG}" != "False" ]];then
        ${FFMPEG_SOURCE_DIR}/configure   ${ff_config} \
            "--as=${ffmpeg_config_as}"                \
            "--cc=${ffmpeg_config_cc}"              \
            "--extra-cflags=${ffmpeg_extra_cflags}"   \
            "--extra-ldflags=${ffmpeg_extra_ldlags}"  \
            --prefix=${ffmpeg_install_dir}
        configure_ret=$?
    fi

     FFMPEG_INSTALL_DIR=${ffmpeg_install_dir}
     FFMPEG_BUILD_DIR=$PWD

    return ${configure_ret}
}

function ffmpeg_build(){
    if [[ "${BUILD}" != "False" ]] || [[ "${BUILD_FFMPEG}" != "False" ]];then
        # 自愈：删除历史中断遗留的 0 字节 .o。clang 被 Ctrl+C/超时杀掉时可能
        # 留下截断对象（mtime 比源码新），make 会误判为最新而跳过重建，
        # 最终链接报成片的 undefined symbol。
        find . -name '*.o' -size 0 -delete 2>/dev/null || true
        # 必须显式检查 make 的返回码：make install 会掩盖 make -j8 的失败
        # （.a 是上次构建的残留时 install 可能成功），导致后续合并链接拿到
        # 残缺的对象树，报一堆 "undefined symbol: ff_xxx_decoder"。
        make -j8 V=1 || { echo "ERROR: ffmpeg make failed (incomplete object tree?)"; exit 1; }
        make install || { echo "ERROR: ffmpeg make install failed"; exit 1; }
    fi
}
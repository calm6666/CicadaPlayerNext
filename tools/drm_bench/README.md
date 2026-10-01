# tools/drm_bench —— DRM 解密的两份实测工具

这里的东西**不是产品代码**，是"回答两个问题"的证据工具：

1. `drm_bench.cpp` —— **性能**：软件解密要占多少 CPU。
2. `cenc_verify.cpp` —— **正确性**：`CENCDecrypter` 在**真的 CENC 产物**上能不能
   把样本逐字节解回明文。

结论与完整的数字表在 `docs/DRM-SOFTWARE-DECRYPT-PERFORMANCE.md`，
这里只讲怎么自己跑一遍。

两者都**链接仓库自己的解密类**（`AES_128Decrypter`、`CENCDecrypter`、
`avAESDecrypt`），所以量出来/验出来的是产品里真正跑的那条路，
而不是某个平行实现。

---

## 1. 编译

需要一个 MSVC 环境（`vcvars64.bat`）和项目自带的 FFmpeg 头/导入库。
注意 **必须带 `/utf-8`**：仓库源码是 UTF-8 无 BOM，MSVC 默认按 cp936 解码会把
中文注释后面紧跟着的代码一起解坏（这是实测踩过的坑：漏了 `/utf-8` 会在
`CENCDecrypter.h` 报一串看着像语法错误的错）。

以下命令里 `%ROOT%` 是仓库根（`D:\hilihili\CicadaPlayerNext`），
`%FF%` 是自带 FFmpeg 的安装前缀（`%ROOT%\external\install\ffmpeg\win32\x86_64`）。

### 性能基准

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /EHsc /MT /O2 /utf-8 ^
   /I "%ROOT%\framework" /I "%FF%\include" ^
   /I "%ROOT%\external\boost" /I "%ROOT%\external\external\ffmpeg" ^
   /I "%ROOT%\external\build\ffmpeg\win32\x86_64" ^
   "%ROOT%\tools\drm_bench\drm_bench.cpp" ^
   "%ROOT%\framework\demuxer\decrypto\avAESDecrypt.cpp" ^
   "%ROOT%\framework\demuxer\play_list\segment_decrypt\AES_128Decrypter.cpp" ^
   "%ROOT%\framework\demuxer\sample_decrypt\CENCDecrypter.cpp" ^
   /Fe:drm_bench.exe /Fo:drm_bench.obj ^
   /link /LIBPATH:"%FF%" libffmpeg.lib ws2_32.lib

set PATH=%FF%;%PATH%
drm_bench.exe
```

再把 `MB/s` 换算成单核占用百分比：

```bat
python tools\drm_bench\core_share.py
```

### CENC 正确性验证

先造一对"同源"的明文/密文文件。**关键**：两个文件必须来自同一次编码，
否则样本边界可能不同、比对就不是同一件事了：

```bat
set FFMPEG=D:\ffmpeg-8.1-full_build\bin\ffmpeg.exe
mkdir %TEMP%\cencreal
%FFMPEG% -y -f lavfi -i testsrc2=size=320x180:rate=25:duration=6 ^
   -c:v libx264 -preset ultrafast -g 25 -pix_fmt yuv420p ^
   -f h264 %TEMP%\cencreal\clear.mp4.h264
:: 上面这一步只是为了拿到编码结果；更直接的是先出 mp4 再重新封装，
:: 但**必须保证两次封装的输入完全一致**，否则样本边界会变。

%FFMPEG% -y -i clear.mp4 -c copy %TEMP%\cencreal\clear.mp4
%FFMPEG% -y -i clear.mp4 -c copy ^
   -encryption_scheme cenc-aes-ctr ^
   -encryption_key 00112233445566778899aabbccddeeff ^
   -encryption_kid 000102030405060708090a0b0c0d0e0f ^
   -movflags +frag_keyframe+empty_moov+default_base_moof %TEMP%\cencreal\cenc.mp4
```

⚠ 已知坑：对**已经分片**的 mp4 再套一次 `-c copy` 加 CENC，**样本边界可能与
源文件不同**（ffmpeg 会重新切分），于是两个文件的第 i 个样本不再是同一个样本。
如果验证结果出现"前若干字节一致、之后整段错"，先怀疑这件事，而不是怀疑解密器。
`cenc_verify.cpp` 会把"样本数 / 样本大小 / 首个错字节"都打出来，正是为了让这个
区分一眼可见。

编译并运行：

```bat
cl /nologo /EHsc /MT /O2 /utf-8 ^
   /I "%ROOT%\framework" /I "%FF%\include" ^
   /I "%ROOT%\external\boost" /I "%ROOT%\external\external\ffmpeg" ^
   /I "%ROOT%\external\build\ffmpeg\win32\x86_64" ^
   "%ROOT%\tools\drm_bench\cenc_verify.cpp" ^
   "%ROOT%\framework\demuxer\sample_decrypt\CENCDecrypter.cpp" ^
   "%ROOT%\framework\demuxer\decrypto\avAESDecrypt.cpp" ^
   /Fe:cenc_verify.exe /Fo:cenc_verify.obj ^
   /link /LIBPATH:"%FF%" libffmpeg.lib ws2_32.lib

set PATH=%FF%;%PATH%
cenc_verify.exe %TEMP%\cencreal\clear.mp4 %TEMP%\cencreal\cenc.mp4 00112233445566778899aabbccddeeff
```

## 2. 怎么读输出

* `BYTE-IDENTICAL to clear` 与 `mismatched` 是两个要点；
  `mismatched == 0 && identical > 0` 才算 PASS。
* 出现 mismatch 时，工具会打印：
  * 该样本的 subsample 表（`clear=` / `protected=`）与总和是否等于样本大小；
  * **四个互相竞争的计数器模型**各自的首个错字节：
    A 连续 / B 每 subsample 重置 / C 整样本连续 / D 同 C 但只用前 8 字节 IV；
  * 多 subsample 时还会把第二个区间的起始 counter 从 30 扫到 50。
  * 这些是为了"把'我的模型错了'变成一个具体的数"，而不是猜。
* FFmpeg 自己的探针会对密文吐一堆 `non-existing PPS` / `SEI truncated`
  之类的 E 日志，并把加密文件"解"出额外样本 —— 那是探针在解随机字节，
  **不是**验证失败。工具里那两个 `readAll`/`readEncrypted` 是拿
  `AV_PKT_DATA_ENCRYPTION_INFO` 的，走的是样本表，不受影响。

## 3. 当前已知结论

见 `docs/DRM-SOFTWARE-DECRYPT-PERFORMANCE.md` §1（性能）与 §2（正确性，
含那 1 个未查清的多 subsample 样本）。

# 内核 JNI 层不需要额外 keep 规则（对外 API 都是显式 JNI 注册/名字查找）
-keep class com.cicada.player.** { *; }
-dontwarn com.cicada.player.**

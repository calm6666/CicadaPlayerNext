package com.cicada.player.compose.upload.network

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonObject

/**
 * ============================================================================
 * JsonNaming.kt —— JSON 键名 驼峰(camelCase) ↔ 下划线(snake_case) 转换工具
 * ============================================================================
 *
 * 背景：后端为 Go 语言，序列化采用下划线风格（如 upload_id、part_number）；
 * 客户端（Kotlin/前端 TS）习惯驼峰风格（如 uploadId、partNumber）。因此约定：
 *
 *  请求方向（客户端 → 后端）：驼峰 → 下划线
 *    - JSON 请求体键名      ：camelToSnake()
 *    - URL 查询参数键名     ：camelToSnake()
 *  响应方向（后端 → 客户端）：下划线 → 驼峰
 *    - 信封 data 的键名     ：snakeToCamel()
 *
 * 注意：对象存储（S3 预签名 PUT）直传不经过本转换器，直接使用标准 HTTP 头
 * （x-amz-checksum-sha256 等），即「对象存储直接用标准命名」。
 *
 * 与前端 @/utils/http/index 拦截器的行为保持一致：只转换「键」，不转换「值」，
 * 嵌套对象/数组递归处理，数字、布尔、字符串原样保留。
 */
object JsonNaming {

    /**
     * 驼峰 → 下划线。
     *
     * 例：
     *   uploadId            → upload_id
     *   partNumber          → part_number
     *   partSha256s         → part_sha256s
     *   partSha256Size      → part_sha256_size
     *   uploadedPartNumbers → uploaded_part_numbers
     *   id / url / parts    → 保持不变
     *
     * 实现说明：两条正则分别处理「小写/数字后接大写」与「连续大写后接小写」
     * （如 partURLs → part_urls）两种边界，最终统一转小写。
     */
    fun camelToSnake(name: String): String = name
        .replace(Regex("([a-z0-9])([A-Z])"), "$1_$2")
        .replace(Regex("([A-Z]+)([A-Z][a-z])"), "$1_$2")
        .lowercase()

    /**
     * 下划线 → 驼峰。
     *
     * 例：
     *   upload_id             → uploadId
     *   uploaded_part_numbers → uploadedPartNumbers
     *   put_url               → putUrl
     *   id / key / parts      → 保持不变
     */
    fun snakeToCamel(name: String): String {
        val parts = name.split('_')
        if (parts.size <= 1) return name
        return buildString {
            append(parts.first())
            for (i in 1 until parts.size) {
                val part = parts[i]
                if (part.isNotEmpty()) append(part.replaceFirstChar { it.uppercase() })
            }
        }
    }

    /**
     * 深度转换 [JsonElement] 树中所有「对象键名」（数组元素继续递归，值不动）。
     *
     * @param element   待转换的 JSON 元素树
     * @param keyMapper 键名映射函数（camelToSnake 或 snakeToCamel）
     * @return 转换后的新元素树（输入元素不被修改）
     */
    fun deepMapKeys(element: JsonElement, keyMapper: (String) -> String): JsonElement = when {
        element.isJsonObject -> {
            val source = element.asJsonObject
            val target = JsonObject()
            for ((key, value) in source.entrySet()) {
                target.add(keyMapper(key), deepMapKeys(value, keyMapper))
            }
            target
        }

        element.isJsonArray -> {
            val source = element.asJsonArray
            val target = JsonArray()
            for (item in source) target.add(deepMapKeys(item, keyMapper))
            target
        }

        else -> element
    }

    /** 将整个对象的键转换为下划线风格（请求体序列化前调用）。 */
    fun toSnakeJsonObject(source: JsonObject): JsonObject =
        deepMapKeys(source, ::camelToSnake) as JsonObject

    /** 将整个对象的键转换为驼峰风格（信封 data 解析后调用）。 */
    fun toCamelJsonObject(source: JsonObject): JsonObject =
        deepMapKeys(source, ::snakeToCamel) as JsonObject
}

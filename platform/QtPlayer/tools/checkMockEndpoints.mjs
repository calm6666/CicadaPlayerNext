#!/usr/bin/env node
// ===========================================================================
// mock 服务接口自检（播放器这一侧真正会用到的那几条链路）
//
// 用法：
//     node tools/checkMockEndpoints.mjs
//     node tools/checkMockEndpoints.mjs http://127.0.0.1:9000
//     set CICADA_MOCK_BASE=http://127.0.0.1:9000 && node tools/checkMockEndpoints.mjs
//
// 退出码：
//     0 = 全部通过（**包含**"preview.bin 空文件 → 预览图不可用"这种合法降级）
//     1 = 有断言失败
//     2 = 9101 / 9000 都没有 mock 服务在跑 → "未实测"（不是失败，但和"通过"要能区分开）
//
// 【为什么放在 platform/QtPlayer/tools/ 而不是 mock-server/】
//   它验的是**播放器这一侧的契约**：Main.qml 拿 view_points / pbp、ProgressRow 拿
//   step_sec 画曲线、进度条预览按 floor(秒/5)+1 取帧 —— 断言里的每一条都对着 Qt 侧的用法。
//   放这儿和 PlayerControlBar/ProgressRow 一起演进；只要机器上有 node 就能跑
//   （mock 自己也是 node，不需要额外依赖）。mock-server/ 那边是"服务"自己的自测，两码事。
//
// 【零依赖】只用 node 内置的 fetch / http / 断言式手写检查，不装任何包。
// ===========================================================================

import http from 'node:http'

/* ---------------------------------------------------------------------------
 * 0. 常量：和 mock-server / Qt 侧一致的几个口径
 * ------------------------------------------------------------------------- */

/* 候选端口：9101 是 mock 默认端口（server.js:70），9000 是常见的手工端口 */
const DEFAULT_CANDIDATES = ['http://127.0.0.1:9101', 'http://127.0.0.1:9000']

/* 预览图"每几秒一帧"（server.js 的 FRAME_INTERVAL_SEC；Qt 侧是 QtPlayerTheme.progressPreviewFrameSec） */
const FRAME_INTERVAL_SEC = 5

/* 预览图里帧与帧之间的分隔符（server.js 的 UNIT_SEP，U+001F） */
const UNIT_SEP = '\u001F'

/* 每个请求的超时；/danmaku/since 会挂起到 25 秒，所以那一条单独给长一点 */
const TIMEOUT_MS = 5000
const LONGPOLL_TIMEOUT_MS = 12000

/* ---------------------------------------------------------------------------
 * 1. 极简测试框架：每条打印 URL + 状态 + 片段，最后汇总
 * ------------------------------------------------------------------------- */
let passed = 0
let failed = 0
const failures = []

function snippet(value, max = 120) {
  const text = typeof value === 'string' ? value : JSON.stringify(value)
  if (text === undefined) return '(undefined)'
  return text.length > max ? text.slice(0, max) + '…' : text
}

function check(name, ok, detail) {
  if (ok) {
    passed += 1
    console.log(`  [OK]   ${name}${detail ? '  —— ' + detail : ''}`)
  } else {
    failed += 1
    failures.push(`${name}${detail ? '  —— ' + detail : ''}`)
    console.log(`  [FAIL] ${name}${detail ? '  —— ' + detail : ''}`)
  }
}

async function getJson(url, timeoutMs = TIMEOUT_MS) {
  const started = Date.now()
  const res = await fetch(url, { signal: AbortSignal.timeout(timeoutMs) })
  const text = await res.text()
  let json = null
  try {
    json = JSON.parse(text)
  } catch {
    /* 不是 JSON 就留 null，由断言去报 */
  }
  return { status: res.status, text, json, ms: Date.now() - started }
}

/* ---------------------------------------------------------------------------
 * 2. 探测：哪个端口在跑（CICADA_MOCK_BASE > 命令行参数 > 默认候选）
 * ------------------------------------------------------------------------- */
async function detectBases() {
  const explicit = (process.argv[2] || process.env.CICADA_MOCK_BASE || '').trim()
  const candidates = explicit ? [explicit.replace(/\/+$/, '')] : DEFAULT_CANDIDATES
  const alive = []

  console.log(`探测可用的 mock 服务（候选：${candidates.join('、')}）`)

  for (const base of candidates) {
    const url = `${base}/healthz`
    try {
      const r = await getJson(url, 2000)
      const ok = r.status === 200 && r.json && r.json.data && r.json.data.ok === true
      console.log(`  ${ok ? '[OK]  ' : '[FAIL]'} GET ${url} → HTTP ${r.status} ${snippet(r.json ?? r.text, 80)}`)
      if (ok) alive.push(base)
    } catch (err) {
      console.log(`  [FAIL] GET ${url} → ${err.name === 'TimeoutError' ? '超时' : '连不上'}（${err.message}）`)
    }
  }

  return alive
}

/* ---------------------------------------------------------------------------
 * 3. 各项接口的检查
 * ------------------------------------------------------------------------- */

/* 1) /healthz —— 已经探过，这里把关键字段打出来（时长决定后面几条的期望值） */
async function checkHealth(base) {
  console.log(`\n== ${base} ==`)
  const url = `${base}/healthz`
  const r = await getJson(url)
  const d = r.json && r.json.data

  check('GET /healthz：HTTP 200 且 data.ok === true',
        r.status === 200 && d && d.ok === true,
        `HTTP ${r.status}，${snippet(d ?? r.text, 140)}`)
  check('GET /healthz：durationSec 是正数',
        d && Number.isFinite(d.durationSec) && d.durationSec > 0,
        `durationSec=${d && d.durationSec}（来源 ${d && d.durationSource}）`)

  return d || {}
}

/* 2) /x/player/v2 —— 分段（视点）：数组、每项 from/to/content、from <= to */
async function checkViewPoints(base, health) {
  const url = `${base}/x/player/v2?aid=&cid=`
  const r = await getJson(url)
  const list = r.json && r.json.data && r.json.data.view_points

  check('GET /x/player/v2：HTTP 200 且 code === 0',
        r.status === 200 && r.json && r.json.code === 0,
        `HTTP ${r.status} ${r.ms}ms，${snippet(r.json ?? r.text)}`)
  check('GET /x/player/v2：view_points 是数组且非空',
        Array.isArray(list) && list.length > 0,
        Array.isArray(list) ? `${list.length} 段：${snippet(list.slice(0, 2))}` : `实际是 ${typeof list}`)

  if (Array.isArray(list) && list.length > 0) {
    const bad = list.filter(p => !Number.isFinite(p.from) || !Number.isFinite(p.to)
                                 || p.from > p.to || typeof p.content !== 'string')
    check('GET /x/player/v2：每项都有 from/to/content 且 from <= to',
          bad.length === 0,
          bad.length === 0 ? `首段 from=${list[0].from} to=${list[0].to} content="${list[0].content}"`
                           : `${bad.length} 项不合格：${snippet(bad.slice(0, 2))}`)
    check('GET /x/player/v2：首段从 0 开始、末段到片长（mock 的约定）',
          list[0].from === 0 && Math.abs(list[list.length - 1].to - health.durationSec) <= 1,
          `from=${list[0].from}，末段 to=${list[list.length - 1].to}，片长=${health.durationSec}`)
  }
}

/* 3) /x/player/pbp —— 高能进度条：step_sec 正数、data 是 0~1 数组 */
async function checkPbp(base, health) {
  const url = `${base}/x/player/pbp?aid=&cid=`
  const r = await getJson(url)
  const d = r.json && r.json.data
  const values = d && d.data

  check('GET /x/player/pbp：HTTP 200 且 code === 0',
        r.status === 200 && r.json && r.json.code === 0,
        `HTTP ${r.status} ${r.ms}ms`)
  check('GET /x/player/pbp：step_sec 是正数',
        d && Number.isFinite(d.step_sec) && d.step_sec > 0,
        `step_sec=${d && d.step_sec}`)
  check('GET /x/player/pbp：data 是 0~1 的数组',
        Array.isArray(values) && values.length > 0
        && values.every(v => Number.isFinite(v) && v >= 0 && v <= 1),
        Array.isArray(values) ? `${values.length} 点：${snippet(values.slice(0, 6))}`
                              : `实际是 ${typeof values}`)

  if (Array.isArray(values) && d && Number.isFinite(d.step_sec) && health.durationSec) {
    const expect = Math.ceil(health.durationSec / d.step_sec)
    check('GET /x/player/pbp：点数 ≈ ceil(片长 / step_sec)',
          Math.abs(values.length - expect) <= 1,
          `期望 ${expect}，实际 ${values.length}`)
  }
}

/* 4) /videoshot/index.json —— 元信息（帧数是 0 也算通过：那是"还没请求过 preview.bin"） */
async function checkVideoshotIndex(base) {
  const url = `${base}/videoshot/index.json`
  const r = await getJson(url)
  const d = r.json && r.json.data

  check('GET /videoshot/index.json：HTTP 200 且 code === 0',
        r.status === 200 && r.json && r.json.code === 0,
        `HTTP ${r.status}，${snippet(d ?? r.text, 160)}`)
  check('GET /videoshot/index.json：pvdata 有尺寸、image/index 是数组',
        d && d.pvdata && Array.isArray(d.image) && Array.isArray(d.index),
        d ? `img=${d.pvdata && d.pvdata.img_x_size}x${d.pvdata && d.pvdata.img_y_size}，` +
            `image=${snippet(d.image)}，index 长度=${d.index && d.index.length}，` +
            `status=${d.mock && d.mock.status}` : '没有 data')
}

/* 5) /videoshot/preview.bin —— 空文件 = 合法降级（不算失败）；有帧就按公式验下标 */
async function checkPreviewBin(base) {
  const url = `${base}/videoshot/preview.bin`
  const r = await getJson(url)
  const text = r.text || ''
  const frames = text.split(UNIT_SEP)

  check('GET /videoshot/preview.bin：HTTP 200',
        r.status === 200,
        `HTTP ${r.status} ${r.ms}ms，content-length≈${Buffer.byteLength(text)} 字节，`
        + `按 \\u001F 切分得 ${frames.length} 段`)

  if (text.length === 0) {
    console.log('  [SKIP] preview.bin 是空文件 → 预览图不可用（mock 没带 --video 或抽帧失败），'
                + '这是**合法降级**，不计失败')
    return
  }

  /* 前几帧抽查：arr[floor(秒/5) + 1]（arr[0] 是空占位，见 server.js:968-978） */
  const probes = [0, 4, 5, 12]
  const results = probes.map(sec => {
    const idx = Math.floor(sec / FRAME_INTERVAL_SEC) + 1
    const frame = frames[idx]
    return { sec, idx, ok: typeof frame === 'string' && frame.startsWith('data:image/') }
  })

  check('GET /videoshot/preview.bin：data URL 帧格式正确（arr[0] 是空占位）',
        frames[0] === '' && results.every(p => p.ok),
        `frames[0]=${JSON.stringify(frames[0])}，` +
        results.map(p => `t=${p.sec}s→arr[${p.idx}]${p.ok ? '✓' : '✗'}`).join('、'))
}

/* 6) 发弹幕 + since 补齐 */
async function checkDanmakuRoundTrip(base) {
  const marker = `自检 ${Date.now()}`
  const sendUrl = `${base}/danmaku/send`
  let seq = null

  try {
    const res = await fetch(sendUrl, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: marker, timeMs: 1000, mode: 1, fontSize: 25, color: 16777215 }),
      signal: AbortSignal.timeout(TIMEOUT_MS),
    })
    const json = await res.json()
    seq = json && json.data && json.data.seq
    check('POST /danmaku/send：HTTP 200 且返回 seq',
          res.status === 200 && json.code === 0 && Number.isFinite(seq),
          `HTTP ${res.status}，${snippet(json)}`)
  } catch (err) {
    check('POST /danmaku/send：HTTP 200 且返回 seq', false, `${err.name}：${err.message}`)
    return
  }

  if (!Number.isFinite(seq)) return

  /* since = seq-1 → 刚发的那条一定在里面，接口会**立即**返回（不会挂起 25 秒） */
  const sinceUrl = `${base}/danmaku/since?seq=${seq - 1}`
  const r = await getJson(sinceUrl, LONGPOLL_TIMEOUT_MS)
  const list = r.json && r.json.data

  check('GET /danmaku/since：拿得到刚发的那条',
        r.status === 200 && Array.isArray(list) && list.some(item => item.text === marker),
        `HTTP ${r.status} ${r.ms}ms，${Array.isArray(list) ? list.length + ' 条：' + snippet(list.slice(-2)) : snippet(r.json ?? r.text)}`)
}

/* 7) SSE：连上 /danmaku/stream，先收到 retry/心跳，再收到一条 data 帧 */
function checkSse(base) {
  return new Promise(resolve => {
    const url = `${base}/danmaku/stream`
    const req = http.get(url, res => {
      let buffer = ''
      let sawHeader = false
      let sawData = false
      let settled = false

      const finish = () => {
        if (settled) return
        settled = true
        clearTimeout(timer)
        req.destroy()

        check('SSE /danmaku/stream：首帧（retry / 心跳注释）到达', sawHeader,
              `HTTP ${res.statusCode}，content-type=${res.headers['content-type']}，首块=${snippet(buffer, 60)}`)
        check('SSE /danmaku/stream：POST 之后收到 data 帧', sawData,
              sawData ? snippet(buffer.slice(buffer.indexOf('data:')), 120) : '只收到注释帧、没有 data 帧')
        resolve()
      }

      res.setEncoding('utf8')
      res.on('data', chunk => {
        buffer += chunk

        if (!sawHeader && /^(retry:|:\s*ping)/m.test(buffer))
          sawHeader = true

        /* 收到 data 帧就收工（服务端在 POST 之后会广播一条） */
        if (buffer.includes('\n\ndata:') || buffer.startsWith('data:')) {
          sawData = true
          finish()
        }
      })
      res.on('end', finish)
      res.on('error', finish)

      const timer = setTimeout(finish, 6000)

      /* 连上之后立刻发一条，触发广播（先连后发，才能保证收到那条 data 帧） */
      setTimeout(() => {
        fetch(`${base}/danmaku/send`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ text: `SSE 自检 ${Date.now()}`, timeMs: 2000, mode: 1 }),
          signal: AbortSignal.timeout(TIMEOUT_MS),
        }).catch(() => { /* 发失败由下一层的断言体现（收不到 data 帧） */ })
      }, 300)
    })

    req.on('error', err => {
      check('SSE /danmaku/stream：建连', false, `连不上：${err.message}`)
      resolve()
    })
    req.setTimeout(TIMEOUT_MS, () => req.destroy(new Error('建连超时')))
  })
}

/* ---------------------------------------------------------------------------
 * 4. 主流程
 * ------------------------------------------------------------------------- */
const bases = await detectBases()

if (bases.length === 0) {
  console.log('\n=== 未实测：9101 / 9000 都没有 mock 服务在跑 ===')
  console.log('先把服务起起来再跑本脚本，例如：')
  console.log('    node front/player/mock-server/server.js --port 9000 --video D:\\movies\\demo.mp4')
  process.exit(2)
}

for (const base of bases) {
  const health = await checkHealth(base)
  await checkViewPoints(base, health)
  await checkPbp(base, health)
  await checkVideoshotIndex(base)
  await checkPreviewBin(base)
  await checkDanmakuRoundTrip(base)
  await checkSse(base)
}

console.log(`\n=== 汇总：${passed} 通过 / ${failed} 失败（测了 ${bases.join('、')}） ===`)
if (failed > 0) {
  console.log('失败项：')
  for (const f of failures) console.log('  - ' + f)
}
process.exit(failed > 0 ? 1 : 0)

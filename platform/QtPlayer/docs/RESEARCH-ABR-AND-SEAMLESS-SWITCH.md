# ABR spec from real source (Shaka `main`, androidx/media `main`, HLS RFC + Apple spec)

`raw.githubusercontent.com` is DNS-blocked in this sandbox, so Shaka quotes come from its published JSDoc **source view of `main`** and ExoPlayer from jsDelivr's raw mirror of `androidx/media@main`. All code quotes are verbatim.

## 1. Constants

**Shaka `SimpleAbrManager`** — [source](https://shaka-project.github.io/shaka-player/docs/api/lib_abr_simple_abr_manager.js.html), defaults in [player_configuration.js](https://shaka-project.github.io/shaka-player/docs/api/lib_util_player_configuration.js.html)

| Name | Default | Meaning |
|---|---|---|
| `abr.switchInterval` | 8 s | Rate limit: at most one switch suggestion per interval |
| `abr.bandwidthUpgradeTarget` | 0.85 | Divisor giving the upper bound `b_next/0.85` |
| `abr.bandwidthDowngradeTarget` | 0.95 | Divisor giving the lower bound `b_item/0.95` |
| `abr.defaultBandwidthEstimate` | 1e6 bps | Used until `minTotalBytes` sampled |
| `abr.minTimeToSwitch` | 0 s | Dwell time after the first good estimate. **No `_minDwellTimeMs` exists in `main`** |
| `abr.cacheLoadThreshold` | 5 ms | Only samples with `deltaTimeMs >= 5` reach the estimator |
| `abr.clearBufferSwitch` | false | ABR switches do **not** clear the SourceBuffer |
| `abr.safeMarginSwitch` | 0 s | Margin passed with the switch |
| `advanced.fastHalfLife` / `slowHalfLife` | 2 / 5 s | EWMA half-lives |
| `advanced.minTotalBytes` / `minBytes` | 128e3 / 16e3 | Trust / discard thresholds |
| `mediaSource.codecSwitchingStrategy` | `RELOAD`, else `SMOOTH` when `device.supportsSmoothCodecSwitching('')` | [enum](https://shaka-project.github.io/shaka-player/docs/api/shaka.config.CodecSwitchingStrategy.html) |

**ExoPlayer `AdaptiveTrackSelection`** — [source](https://cdn.jsdelivr.net/gh/androidx/media@main/libraries/exoplayer/src/main/java/androidx/media3/exoplayer/trackselection/AdaptiveTrackSelection.java) ([canonical](https://github.com/androidx/media/blob/main/libraries/exoplayer/src/main/java/androidx/media3/exoplayer/trackselection/AdaptiveTrackSelection.java))

| Name | Default | Meaning |
|---|---|---|
| `DEFAULT_MIN_DURATION_FOR_QUALITY_INCREASE_MS` | 10_000 | Buffer needed to switch up |
| `DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS` | 25_000 | Above this, defer a down-switch |
| `DEFAULT_MIN_DURATION_TO_RETAIN_AFTER_DISCARD_MS` | 25_000 | Lead-in kept when discarding |
| `DEFAULT_MAX_WIDTH_TO_DISCARD` / `HEIGHT` | 1279 / 719 | Only ≤720p chunks may be discarded |
| `DEFAULT_BANDWIDTH_FRACTION` | 0.7f | Fraction of estimate deemed usable |
| `DEFAULT_BUFFERED_FRACTION_TO_LIVE_EDGE_FOR_QUALITY_INCREASE` | 0.75f | Live up-switch alternative |
| `MIN_TIME_BETWEEN_BUFFER_REEVALUTATION_MS` | 1000 | Queue re-evaluation rate limit |

**Shaka EWMA** ([estimator](https://shaka-project.github.io/shaka-player/docs/api/lib_abr_ewma_bandwidth_estimator.js.html), [Ewma](https://shaka-project.github.io/shaka-player/docs/api/lib_abr_ewma.js.html)): `alpha = exp(ln(0.5)/halfLife)`; `adjAlpha = alpha^weight`; `estimate = value*(1-adjAlpha) + adjAlpha*estimate`; `value = 8000*numBytes/durationMs`; `weight = durationMs/1000`; `getEstimate() = estimate/(1-alpha^totalWeight)`; `getBandwidthEstimate() = min(fast, slow)`.

## 2. Mechanisms

**(1) Minimum dwell time.** Shaka gates the callback: `if (delta < this.config_.switchInterval * 1000) { return; }` where `delta = now - this.lastTimeChosenMs_`. At startup it back-dates the timer so the first switch waits only `minTimeToSwitch`: `this.lastTimeChosenMs_ -= (this.config_.switchInterval - this.config_.minTimeToSwitch) * 1000;`. `chooseVariant()` ends with `this.lastTimeChosenMs_ = Date.now();`. ExoPlayer has no dwell timer; its analogue is `MIN_TIME_BETWEEN_BUFFER_REEVALUTATION_MS = 1000` gating `evaluateQueueSize`.

**(2) Up-switch vs down-switch gating.** Shaka accepts variant `i` iff the estimate sits inside a two-sided window:

```js
const minBandwidth = itemBandwidth / this.config_.bandwidthDowngradeTarget;
const maxBandwidth = nextBandwidth / this.config_.bandwidthUpgradeTarget;
if (chosen && item && currentBandwidth >= minBandwidth &&
    currentBandwidth <= maxBandwidth && ...) { chosen = item; }
```

The loop keeps the highest accepted variant. *Derived from the code (undocumented):* with the shipped `0.85 < 0.95`, `b_next/0.85 = 1.176·b_next` is always looser than the next variant's own `b_next/0.95 = 1.053·b_next`, so the upper bound never binds — effectively “highest `i` with `est ≥ b_i/0.95`”: 5.3 % headroom and **no** hysteresis. Shaka's asymmetry is temporal; set `bandwidthUpgradeTarget > bandwidthDowngradeTarget` to obtain a real band.

ExoPlayer gates on buffer instead (lower index = higher quality):

```java
if (newSelectedIndex < previousSelectedIndex
    && bufferedDurationUs < minDurationForQualityIncreaseUs) {
  newSelectedIndex = previousSelectedIndex;          // defer switch up
} else if (newSelectedIndex > previousSelectedIndex
    && bufferedDurationUs >= maxDurationForQualityDecreaseUs && ...) {
  newSelectedIndex = previousSelectedIndex;          // defer switch down
}
```

Estimate: `cautiousBandwidthEstimate = latestBitrateEstimate * bandwidthFraction` (0.7), then TTFB-aware `cautiousBw * max(chunkDuration/playbackSpeed - ttfb, 0) / chunkDuration`; the ideal track is the one passing `canSelectFormat(format, format.bitrate, effectiveBitrate)` (`trackBitrate <= effectiveBitrate`). Live up-switch buffer is `min(availableDurationUs * 0.75, 10 s)`.

**(3) Segment boundary; what is kept or discarded.** An ABR switch passes `clearBuffer = abr.clearBufferSwitch = false`, and `switchInternal_` keeps all buffered data, re-pointing the stream and refetching from the boundary after the last appended segment:

```js
time = mediaState.lastSegmentReference.endTime;
mediaState.segmentIterator = mediaState.stream.segmentIndex.getIteratorForTime(
    time, /* allowNonIndependent= */ false, reverse);
```

So the new variant's segments are appended to the **same** SourceBuffer. Clearing happens only when requested (`clearBuffer_(mediaState, /* flush= */ true, safeMargin)`) or when switching muxed↔alternate audio, which forces `forceClearBuffer_(otherState)` plus an MSE reload. Codec changes follow the enum: `change_()` runs `this.sourceBuffers_.get(contentType).changeType(type);` when `device.supportsSmoothCodecSwitching(keySystem)` — **no flush, no reload** — otherwise `reset_()` runs, “Resets the MediaSource and re-adds source buffers due to codec mismatch”, restoring duration and `currentTime`. Flushing is legal only on an empty buffer:

```js
goog.asserts.assert(this.video_.buffered.length == 0,
    'MediaSourceEngine.flush_ should only be used after clearing all data!');
this.video_.currentTime -= 0.001;   // seeking forces the pipeline to be flushed
```

`abort_()` calls `sourceBuffer.abort()` to “reset MSE's last_decode_timestamp on all track buffers, which should trigger the splicing logic for overlapping segments.” ExoPlayer inverts this: `evaluateQueueSize` returns an index and earlier chunks are dropped, but only if the retained lead-in ≥ 25 s and the chunk is lower bitrate, ≤1279×719 and lower height than ideal. Sources: [streaming_engine.js](https://shaka-project.github.io/shaka-player/docs/api/lib_media_streaming_engine.js.html), [media_source_engine.js](https://shaka-project.github.io/shaka-player/docs/api/lib_media_source_engine.js.html).

**(4) What `EXT-X-INDEPENDENT-SEGMENTS` buys you.** [RFC 8216bis-22 §4.4.2.1](https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis-22): “The EXT-X-INDEPENDENT-SEGMENTS tag indicates that all media samples in a Media Segment can be decoded without information from other segments. It applies to every Media Segment in the Playlist.” … “If [it] appears in a Multivariant Playlist, it applies to every Media Segment in every Media Playlist in the Multivariant Playlist.” That licenses decoding from any segment boundary, so a switch can take effect at the next segment with no pre-roll. It does **not** guarantee cross-variant alignment; that is [RFC 8216 §6.2.4](https://www.rfc-editor.org/rfc/rfc8216.txt): “The server MUST meet the following constraints when producing Variant Streams in order to allow clients to switch between them seamlessly: Each Variant Stream MUST present the same content. Matching content in Variant Streams MUST have matching timestamps. … MUST have matching Discontinuity Sequence Numbers. … Each Media Playlist in each Variant Stream MUST have the same target duration.” Client policy is explicitly out of scope (§6.3.1: “Algorithms used by the client to switch between Variant Streams are beyond the scope of this document.”).

**No text in 8216bis-22 or RFC 8216 states “switching can occur at any segment boundary”, and neither requires aligned segments** — that phrasing is not normative HLS. [Apple's authoring spec](https://developer.apple.com/documentation/http-live-streaming/hls-authoring-specification-for-apple-devices): 9.11 “If your video segments start with an IDR, you SHOULD use the `EXT-X-INDEPENDENT-SEGMENTS` tag in the Multivariant Playlist.”; 9.12 otherwise “you MUST use the `EXT-X-INDEPENDENT-SEGMENTS` tag in all video Media Playlists”; 1.13 “Key frames (IDRs) SHOULD be present every two seconds.”; 16.3 spatial↔non-spatial switches “MUST be marked with an EXT-X-DISCONTINUITY tag, or the Media Initialization Section MUST contain sample descriptions for both kinds of content.” **Apple item 7.4 was NOT retrieved** (JS-rendered page, single-line JSON backing blob); it is not guessed here.

## 3. Implementable ABR for a native C++ player with a segment-based DASH/HLS demuxer

**Constants** (provenance): `FAST_HALF_LIFE=2.0s`, `SLOW_HALF_LIFE=5.0s`, `MIN_SAMPLE_BYTES=16000`, `MIN_TOTAL_BYTES=128000`, `DEFAULT_ESTIMATE=1e6bps`, `MIN_DWELL=8s` [Shaka]; `BANDWIDTH_FRACTION=0.7`, `BUF_UP=10s`, `BUF_DOWN=25s`, `BUF_KEEP=25s`, `MAX_DISCARD=1279x719` [ExoPlayer]; `SAFETY_UP=1.30` / `SAFETY_DOWN=1.00` (deliberately `UP>DOWN`, unlike Shaka's defaults, to create the hysteresis they lack); `BUF_PANIC=4s`; require identical `EXT-X-TARGETDURATION` across HLS variants [RFC 8216 §6.2.4].

**State:** `cur`, `last_switch_ts`, `bytes_sampled`, `fast`, `slow`.

**Per completed segment download:** if `bytes >= MIN_SAMPLE_BYTES`: `bw = 8*bytes/seconds`; `w = seconds`; `fast.sample(w,bw)`; `slow.sample(w,bw)`; `bytes_sampled += bytes`. `throughput = bytes_sampled >= MIN_TOTAL_BYTES ? min(fast.est(), slow.est()) : DEFAULT_ESTIMATE` [Shaka]. `usable = 0.7*throughput`, or when TTFB and segment duration are known, `usable = 0.7*throughput * max(seg_dur/rate - ttfb, 0)/seg_dur` [ExoPlayer].

**Decision, evaluated only at a segment boundary:**

1. `buf = buffer_end - playhead`. If `buf < BUF_PANIC` → switch down now, bypassing dwell.
2. If `now - last_switch_ts < MIN_DWELL` → return [Shaka].
3. `cand` = highest variant with `bitrate <= usable` [ExoPlayer `canSelectFormat`].
4. `cand == cur` → return.
5. **Up** (`cand > cur`): require `throughput >= bitrate(cand) * SAFETY_UP` **and** `buf >= BUF_UP`; else return.
6. **Down** (`cand < cur`): allow if `throughput < bitrate(cur) * SAFETY_DOWN` **or** `buf < BUF_UP/2`; otherwise defer.
7. **Apply:** identical codec string and init segment → stop only the not-yet-started fetch, keep all appended data, continue at the next boundary in the same decoder/queue [Shaka `switchInternal_`, no flush]. Different codec string → reconfigure in place where supported [Shaka `changeType`/`SMOOTH`], else rebuild and restore position [Shaka `reset_`/`RELOAD`]. Muxing-mode change (muxed↔separate audio) → forced flush/reload.
8. Optional, to accelerate an up-switch: discard queued segments before index `i` only if the retained lead-in `>= BUF_KEEP` and each discarded chunk has `bitrate < ideal.bitrate`, `height <= 719`, `width <= 1279` [ExoPlayer].
9. `cur = cand`; `last_switch_ts = now`.

**Invariants:** never act mid-segment; require `EXT-X-INDEPENDENT-SEGMENTS` (or an IDR-bearing / `EXT-X-DISCONTINUITY` switch point) for HLS; never flush a same-codec switch; never discard already-decoded data except by step 8.

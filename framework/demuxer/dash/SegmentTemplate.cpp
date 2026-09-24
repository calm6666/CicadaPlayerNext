//
// Created by yuyuan on 2021/03/09.
//

#include "SegmentTemplate.h"
#include "SegmentInformation.h"
#include "SegmentTimeline.h"
#include "demuxer/play_list/playList.h"
#include "utils/timer.h"
#include <algorithm>
#include <time.h>

using namespace Cicada;
using namespace Cicada::Dash;

SegmentTemplateSegment::SegmentTemplateSegment(IDashUrl *parent) : DashSegment(parent)
{
    templated = true;
    templ = nullptr;
}

SegmentTemplateSegment::~SegmentTemplateSegment()
{}

void SegmentTemplateSegment::setSourceUrl(const std::string &url)
{
    sourceUrl = DashUrl(DashUrl::Component(url, templ));
}

void SegmentTemplateSegment::setParentTemplate(SegmentTemplate *templ_)
{
    templ = templ_;
}

SegmentTemplate::SegmentTemplate(SegmentTemplateSegment *seg, SegmentInformation *parent)
    : ISegmentBase(parent, AbstractAttr::Type::SegmentTemplate)
{
    setInitSegment(nullptr);
    parentSegmentInformation = parent;
    virtualsegment = seg;
    virtualsegment->setParent(parentSegmentInformation);
    virtualsegment->setParentTemplate(this);
}

SegmentTemplate::~SegmentTemplate()
{
    delete virtualsegment;
}

void SegmentTemplate::setSourceUrl(const std::string &url)
{
    virtualsegment->setSourceUrl(url);
}

void SegmentTemplate::pruneByPlaybackTime(int64_t time)
{
    AbstractAttr *p = getAttribute(Type::Timeline);
    if (p) {
        return static_cast<SegmentTimeline *>(p)->pruneByPlaybackTime(time);
    }
}

size_t SegmentTemplate::pruneBySequenceNumber(uint64_t number)
{
    AbstractAttr *p = getAttribute(Type::Timeline);
    if (p) {
        return static_cast<SegmentTimeline *>(p)->pruneBySequenceNumber(number);
    }
    return 0;
}

uint64_t SegmentTemplate::getLiveTemplateNumber(int64_t playbacktime, bool abs) const
{
    uint64_t number = inheritStartNumber();
    /* live streams / templated */
    const int64_t dur = inheritDuration();
    if (dur) {
        /* compute, based on current time */
        /* N = (T - AST - PS - D)/D + sSN */
        const Timescale timescale = inheritTimescale();
        if (abs) {
            int64_t streamstart = parentSegmentInformation->getPlayList()->availabilityStartTime;
            streamstart += parentSegmentInformation->getPeriodStart();
            playbacktime -= streamstart;
        }
        if (playbacktime < 0) {
            playbacktime = 0;
        }
        int64_t elapsed = timescale.ToScaled(playbacktime) - dur;
        if (elapsed > 0) {
            number += elapsed / dur;
        }
    }

    return number;
}

int64_t SegmentTemplate::getMinAheadTime(uint64_t number) const
{
    SegmentTimeline *timeline = inheritSegmentTimeline();
    if (timeline) {
        const Timescale timescale = timeline->inheritTimescale();
        return timescale.ToTime(timeline->getMinAheadScaledTime(number));
    } else {
        const Timescale timescale = inheritTimescale();
        uint64_t current = getLiveTemplateNumber(af_get_utc_time());
        int64_t i_length = (current - number) * inheritDuration();
        return timescale.ToTime(i_length);
    }
}

DashSegment *SegmentTemplate::getMediaSegment(uint64_t number) const
{
    const SegmentTimeline *tl = inheritSegmentTimeline();
    if (tl == nullptr || (tl->maxElementNumber() >= number && tl->minElementNumber() <= number)) {
        return virtualsegment;
    }
    return nullptr;
}

DashSegment *SegmentTemplate::getInitSegment() const
{
    return ISegmentBase::getInitSegment();
}

DashSegment *SegmentTemplate::getNextMediaSegment(uint64_t i_pos, uint64_t *pi_newpos, bool *pb_gap) const
{
    *pb_gap = false;
    *pi_newpos = i_pos;
    /* Check if we don't exceed timeline */
    const SegmentTimeline *timeline = inheritSegmentTimeline();
    if (timeline) {
        *pi_newpos = std::max(timeline->minElementNumber(), i_pos);
        if (timeline->maxElementNumber() < i_pos) {
            return nullptr;
        }
    } else {
        /* check template upper bound */
        const playList *playlist = parentSegmentInformation->getPlayList();
        const Timescale timescale = inheritTimescale();
        const int64_t segmentduration = inheritDuration();
        int64_t totalduration = parentSegmentInformation->getPeriodDuration();
        if (totalduration == 0) {
            totalduration = playlist->getDuration();
        }
        if (totalduration && segmentduration) {
            /* endnum 是**排他**上界：合法编号是 [startNumber, endnum) ，
             * 也就是最后一个合法编号 = endnum - 1。 */
            uint64_t endnum = inheritStartNumber() + (timescale.ToScaled(totalduration) + segmentduration - 1) / segmentduration;

            /*
             * 【2026-09-21 修两个 bug —— "seek 到最后少一段、直接跳结尾" 的根因】
             *
             * 1) 原来是 `if (i_pos >= endnum - 1) return nullptr;` —— 把**最后一个
             *    合法分片**（endnum - 1）也判成了 EOS。实测片源
             *    `mediaPresentationDuration="PT4M28.2S" duration="10000000" endNumber="27"`
             *    的 27 个分片里，第 27 个（[260s, 268.2s)，只有 8.2 秒的余量分片）
             *    就是这样被丢掉的：播完第 26 个分片去要第 27 个 → 返回 nullptr →
             *    `DashStream::updateSegment()` 打 "EOS"、`mIsDataEOS = true` →
             *    `Player ReadPacket EOF` → 播放器直接跳到结尾。
             *
             * 2) manifest 里的 `endNumber` 从来没被解析过。duration 只能算"分片边界"，
             *    算不出"一共有几片"：最后一个余量分片时长不足 duration 时，按
             *    duration 反推会多算出一片（27 片却算出 28）。现在两头取严：
             *    endNumber 给了就用它当权威上界。
             */
            const uint64_t manifestEndNumber = inheritEndNumber();

            if (manifestEndNumber > 0 && manifestEndNumber + 1 < endnum) {
                endnum = manifestEndNumber + 1;
            }

            if (i_pos >= endnum) {
                *pi_newpos = i_pos;
                return nullptr;
            }
        }
        *pi_newpos = i_pos;
        /* start number */
        *pi_newpos = std::max(inheritStartNumber(), i_pos);
    }
    return virtualsegment;
}

uint64_t SegmentTemplate::getStartSegmentNumber() const
{
    const SegmentTimeline *timeline = inheritSegmentTimeline();
    return timeline ? timeline->minElementNumber() : inheritStartNumber();
}

bool SegmentTemplate::getSegmentNumberByTime(int64_t time, uint64_t *ret) const
{
    const SegmentTimeline *timeline = inheritSegmentTimeline();
    if (timeline) {
        const Timescale timescale = timeline->inheritTimescale();
        int64_t st = timescale.ToScaled(time);
        *ret = timeline->getElementNumberByScaledPlaybackTime(st);
        return true;
    }

    const int64_t duration = inheritDuration();
    if (duration && mParent) {
        playList *playlist = mParent->getPlayList();
        if (playlist->isLive()) {
            int64_t now = af_get_utc_time();
            if (playlist->availabilityStartTime) {
                if (time >= playlist->availabilityStartTime && time < now) {
                    *ret = getLiveTemplateNumber(time, true);
                } else if (now - playlist->availabilityStartTime > time) {
                    *ret = getLiveTemplateNumber(time, false);
                }
            } else {
                return false;
            }
        } else {
            const Timescale timescale = inheritTimescale();
            *ret = inheritStartNumber();
            *ret += timescale.ToScaled(time) / duration;

            /*
             * 【2026-09-21 修：把结果夹在合法范围内】
             *
             * 时间落在最后一个"余量分片"里（或干脆超过媒体时长）时，按 duration 整除
             * 会算出一个**不存在**的编号（实测 seek 到 256.8s、27 片里最后一片是
             * [260,268.2) 时算出的编号 26 是好的，但 seek 到 260.7s 算出 27、再往下
             * 算到 28 就已经不存在）。不存在的编号会去请求 404，DashStream 接着往后
             * 找下一片、找不到就报 EOS —— 用户看到的就是"少一段直接跳结尾"。
             * 夹到 endNumber 之后，越界的 seek 会落在真正的最后一片上。
             */
            const uint64_t manifestEndNumber = inheritEndNumber();

            if (manifestEndNumber > 0 && *ret > manifestEndNumber) {
                *ret = manifestEndNumber;
            }
        }
        return true;
    }

    return false;
}


bool SegmentTemplate::getPlaybackTimeDurationBySegmentNumber(uint64_t number, int64_t *time, int64_t *duration) const
{
    if (number == std::numeric_limits<uint64_t>::max()) {
        return false;
    }

    Timescale timescale;
    int64_t stime, sduration;

    const SegmentTimeline *timeline = inheritSegmentTimeline();
    if (timeline) {
        timescale = timeline->inheritTimescale();
        if (!timeline->getScaledPlaybackTimeDurationBySegmentNumber(number, &stime, &sduration)) {
            return false;
        }
    } else {
        timescale = inheritTimescale();
        uint64_t startNumber = inheritStartNumber();
        if (number < startNumber) {
            return false;
        }
        sduration = inheritDuration();
        stime = (number - startNumber) * sduration;
    }

    *time = timescale.ToTime(stime);
    *duration = timescale.ToTime(sduration);
    return true;
}

SegmentTemplateInit::SegmentTemplateInit(SegmentTemplate *templ_, IDashUrl *parent) : DashSegment(parent)
{
    templ = templ_;
}

SegmentTemplateInit::~SegmentTemplateInit()
{}

void SegmentTemplateInit::setSourceUrl(const std::string &url)
{
    sourceUrl = DashUrl(DashUrl::Component(url, templ));
}
//
// Created for the CicadaPlayerNext cmdline demo (Windows).
//

#include "SdlControlBar.h"

#include <algorithm>

namespace {
    /* Layout constants, in drawable pixels. */
    const int BAR_HEIGHT = 56;
    const int BAR_MARGIN = 12;
    const int BUTTON_SIZE = 36;
    const int SLIDER_HEIGHT = 6;
    const int KNOB_SIZE = 14;
    const int VOLUME_TRACK_WIDTH = 110;
    const int MIN_PROGRESS_WIDTH = 80;

    /* Milliseconds of no mouse activity before the bar hides itself. */
    const Uint32 HIDE_DELAY_MS = 3000;

    /* Colours. */
    const SDL_Color COLOR_BAR_BG = { 16, 16, 16, 200 };
    const SDL_Color COLOR_CONTROL = { 235, 235, 235, 255 };
    const SDL_Color COLOR_HIGHLIGHT = { 90, 190, 255, 255 };
    const SDL_Color COLOR_TRACK = { 90, 90, 90, 255 };

    float clamp01(float value)
    {
        if (value < 0.0f) {
            return 0.0f;
        }

        if (value > 1.0f) {
            return 1.0f;
        }

        return value;
    }

    bool pointIn(const SDL_Rect &rect, int x, int y)
    {
        return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    }

    /* Canvas backed by an SDL_Renderer. */
    class SdlRendererCanvas : public SdlControlBar::Canvas {
    public:
        explicit SdlRendererCanvas(SDL_Renderer *renderer) : mRenderer(renderer)
        {
            /* Blend so the bar dims the video underneath instead of hiding it. */
            SDL_SetRenderDrawBlendMode(mRenderer, SDL_BLENDMODE_BLEND);
        }

        ~SdlRendererCanvas() override
        {
            SDL_SetRenderDrawBlendMode(mRenderer, SDL_BLENDMODE_NONE);
        }

        void fill(const SDL_Rect &rect, const SDL_Color &color) override
        {
            SDL_SetRenderDrawColor(mRenderer, color.r, color.g, color.b, color.a);
            SDL_RenderFillRect(mRenderer, &rect);
        }

    private:
        SDL_Renderer *mRenderer;
    };

    /*
     * Canvas backed by a BGRA bitmap, for the zero-copy D3D11 path. The bitmap
     * only covers the bar itself, while the drawing code works in window
     * coordinates, so (mOffsetX, mOffsetY) translates one into the other.
     *
     * That path copies the bitmap into the back buffer without blending, so the
     * alpha is composited here, against black, and then forced opaque - the bar
     * looks the same as it does over the video on the SDL path.
     */
    class BgraBitmapCanvas : public SdlControlBar::Canvas {
    public:
        BgraBitmapCanvas(uint32_t *pixels, int width, int height, int offsetX, int offsetY)
            : mPixels(pixels), mWidth(width), mHeight(height), mOffsetX(offsetX), mOffsetY(offsetY)
        {
            for (int i = 0; i < width * height; ++i) {
                mPixels[i] = 0xff000000u;
            }
        }

        void fill(const SDL_Rect &rect, const SDL_Color &color) override
        {
            const int x0 = std::max(0, rect.x - mOffsetX);
            const int y0 = std::max(0, rect.y - mOffsetY);
            const int x1 = std::min(mWidth, rect.x - mOffsetX + rect.w);
            const int y1 = std::min(mHeight, rect.y - mOffsetY + rect.h);
            const uint32_t a = color.a;

            for (int y = y0; y < y1; ++y) {
                uint32_t *row = mPixels + y * mWidth;

                for (int x = x0; x < x1; ++x) {
                    const uint32_t dst = row[x];

                    const uint32_t r = (color.r * a + ((dst >> 16) & 0xff) * (255 - a)) / 255;
                    const uint32_t g = (color.g * a + ((dst >> 8) & 0xff) * (255 - a)) / 255;
                    const uint32_t b = (color.b * a + (dst & 0xff) * (255 - a)) / 255;

                    row[x] = 0xff000000u | (r << 16) | (g << 8) | b;
                }
            }
        }

    private:
        uint32_t *mPixels;
        int mWidth;
        int mHeight;
        int mOffsetX;
        int mOffsetY;
    };
}

void SdlControlBar::layout(int windowWidth, int windowHeight)
{
    std::lock_guard<std::mutex> lock(mMutex);

    mBar.x = 0;
    mBar.w = windowWidth;
    mBar.h = BAR_HEIGHT;
    mBar.y = windowHeight - BAR_HEIGHT;

    const int centerY = mBar.y + BAR_HEIGHT / 2;
    const int buttonTop = centerY - BUTTON_SIZE / 2;

    mPlayPause.x = BAR_MARGIN;
    mPlayPause.y = buttonTop;
    mPlayPause.w = BUTTON_SIZE;
    mPlayPause.h = BUTTON_SIZE;

    mFullScreen.w = BUTTON_SIZE;
    mFullScreen.h = BUTTON_SIZE;
    mFullScreen.x = windowWidth - BAR_MARGIN - BUTTON_SIZE;
    mFullScreen.y = buttonTop;

    mVolume.w = VOLUME_TRACK_WIDTH;
    mVolume.h = SLIDER_HEIGHT;
    mVolume.x = mFullScreen.x - BAR_MARGIN - VOLUME_TRACK_WIDTH;
    mVolume.y = centerY - SLIDER_HEIGHT / 2;

    const int progressLeft = mPlayPause.x + mPlayPause.w + BAR_MARGIN;
    const int progressRight = mVolume.x - BAR_MARGIN;

    mProgress.x = progressLeft;
    mProgress.w = progressRight - progressLeft;

    if (mProgress.w < MIN_PROGRESS_WIDTH) {
        /* Very narrow window: keep the bar drawable rather than letting the
         * progress bar invert. The sliders simply overlap in that case. */
        mProgress.w = MIN_PROGRESS_WIDTH;
    }

    mProgress.h = SLIDER_HEIGHT;
    mProgress.y = centerY - SLIDER_HEIGHT / 2;
}

SdlControlBar::Hit SdlControlBar::mouseDown(int x, int y)
{
    std::lock_guard<std::mutex> lock(mMutex);

    mLastActivity = SDL_GetTicks();

    /*
     * The bar hides itself three seconds after the last mouse activity. Keep
     * swallowing clicks while it is hidden: the controls are not on screen, so
     * acting on them turns "click the picture" into a pause, a seek or a full
     * screen switch the user never asked for.
     *
     * The activity stamp above still runs first, so this very click brings the
     * bar back and the next one hit tests against visible buttons.
     */
    if (!visibleLocked()) {
        return Hit::None;
    }

    if (!pointIn(mBar, x, y)) {
        return Hit::None;
    }

    if (pointIn(mPlayPause, x, y)) {
        return Hit::PlayPause;
    }

    if (pointIn(mFullScreen, x, y)) {
        return Hit::FullScreen;
    }

    /* Grab the sliders generously: the visible track is only a few pixels tall,
     * so a click a little above or below it still starts the drag. */
    SDL_Rect progressHit = mProgress;
    progressHit.y -= KNOB_SIZE;
    progressHit.h += KNOB_SIZE * 2;

    if (pointIn(progressHit, x, y)) {
        mDrag = Hit::Progress;
        mDragFraction = fractionAtLocked(Hit::Progress, x);
        return Hit::Progress;
    }

    SDL_Rect volumeHit = mVolume;
    volumeHit.y -= KNOB_SIZE;
    volumeHit.h += KNOB_SIZE * 2;

    if (pointIn(volumeHit, x, y)) {
        mDrag = Hit::Volume;
        mDragFraction = fractionAtLocked(Hit::Volume, x);
        return Hit::Volume;
    }

    /* Clicking the bar itself just keeps it visible. */
    return Hit::None;
}

void SdlControlBar::mouseMotion(int x, int y)
{
    std::lock_guard<std::mutex> lock(mMutex);

    mHovered = pointIn(mBar, x, y);

    if (mDrag != Hit::None) {
        mDragFraction = fractionAtLocked(mDrag, x);
    }

    if (mHovered || mDrag != Hit::None) {
        mLastActivity = SDL_GetTicks();
    }
}

void SdlControlBar::mouseUp()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mDrag = Hit::None;
}

void SdlControlBar::mouseLeave()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mHovered = false;
}

bool SdlControlBar::isDragging() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mDrag != Hit::None;
}

SdlControlBar::Hit SdlControlBar::dragControl() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mDrag;
}

float SdlControlBar::dragFraction() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mDragFraction;
}

float SdlControlBar::fractionAt(Hit control, int x) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return fractionAtLocked(control, x);
}

float SdlControlBar::fractionAtLocked(Hit control, int x) const
{
    const SDL_Rect &track = (control == Hit::Volume) ? mVolume : mProgress;

    if (track.w <= 0) {
        return 0.0f;
    }

    return clamp01(static_cast<float>(x - track.x) / static_cast<float>(track.w));
}

bool SdlControlBar::contains(int x, int y) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return pointIn(mBar, x, y);
}

void SdlControlBar::notifyActivity()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mLastActivity = SDL_GetTicks();
}

bool SdlControlBar::visible() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return visibleLocked();
}

bool SdlControlBar::visibleLocked() const
{
    if (mDrag != Hit::None || mHovered) {
        return true;
    }

    return (SDL_GetTicks() - mLastActivity) < HIDE_DELAY_MS;
}

void SdlControlBar::drawTriangle(Canvas &canvas, const SDL_Rect &area, SDL_Color color) const
{
    /*
     * Filled right pointing triangle, drawn as one horizontal span per row.
     * SDL_RenderGeometry() would be shorter but only exists from 2.0.18; this
     * keeps the bar portable across whatever SDL2 the build picks up - and it
     * works unchanged on the bitmap canvas, which only knows rectangles.
     */
    const int height = area.h - 8;
    const int top = area.y + (area.h - height) / 2;
    const int left = area.x + 8;
    const int width = area.w - 12;

    for (int row = 0; row < height; ++row) {
        /* Widest at the vertical middle, so the triangle is symmetric. */
        int span = (row * 2 <= height) ? (row * 2 * width / height)
                                       : ((height - row) * 2 * width / height);

        if (span <= 0) {
            continue;
        }

        const SDL_Rect line{ left, top + row, span, 1 };
        canvas.fill(line, color);
    }
}

void SdlControlBar::drawPlayPause(Canvas &canvas, const SDL_Rect &area, bool paused, bool highlight) const
{
    const SDL_Color color = highlight ? COLOR_HIGHLIGHT : COLOR_CONTROL;

    if (paused) {
        drawTriangle(canvas, area, color);
        return;
    }

    /* Pause: two vertical bars. */
    const int barWidth = 6;
    const int gap = 7;
    const int height = area.h - 8;
    const int top = area.y + (area.h - height) / 2;
    const int left = area.x + (area.w - (barWidth * 2 + gap)) / 2;

    const SDL_Rect first{ left, top, barWidth, height };
    const SDL_Rect second{ left + barWidth + gap, top, barWidth, height };
    canvas.fill(first, color);
    canvas.fill(second, color);
}

void SdlControlBar::drawFullScreen(Canvas &canvas, const SDL_Rect &area, bool fullScreen) const
{
    (void) fullScreen;

    /* Four corner brackets. */
    const SDL_Color color = COLOR_CONTROL;
    const int armLength = 10;
    const int thickness = 2;
    const int inset = 8;

    const int left = area.x + inset;
    const int right = area.x + area.w - inset;
    const int top = area.y + inset;
    const int bottom = area.y + area.h - inset;

    /* One horizontal and one vertical arm per corner. */
    const SDL_Rect rects[8] = {
        { left, top, armLength, thickness },
        { left, top, thickness, armLength },
        { right - armLength, top, armLength, thickness },
        { right - thickness, top, thickness, armLength },
        { left, bottom - thickness, armLength, thickness },
        { left, bottom - armLength, thickness, armLength },
        { right - armLength, bottom - thickness, armLength, thickness },
        { right - thickness, bottom - armLength, thickness, armLength },
    };

    for (const SDL_Rect &rect : rects) {
        canvas.fill(rect, color);
    }
}

void SdlControlBar::drawSlider(Canvas &canvas, const SDL_Rect &track, float fraction, bool highlight) const
{
    canvas.fill(track, COLOR_TRACK);

    SDL_Rect filled = track;
    filled.w = static_cast<int>(track.w * clamp01(fraction));

    if (filled.w > 0) {
        canvas.fill(filled, highlight ? COLOR_HIGHLIGHT : COLOR_CONTROL);
    }

    /* Knob, centred on the current position. */
    const int knobX = track.x + static_cast<int>(track.w * clamp01(fraction)) - KNOB_SIZE / 2;
    const SDL_Rect knob{ knobX, track.y + track.h / 2 - KNOB_SIZE / 2, KNOB_SIZE, KNOB_SIZE };
    canvas.fill(knob, highlight ? COLOR_HIGHLIGHT : COLOR_CONTROL);
}

/* mMutex already held: the whole visual design lives here, shared by both paths. */
void SdlControlBar::drawBar(Canvas &canvas, const Info &info) const
{
    if (mBar.w <= 0) {
        return;
    }

    canvas.fill(mBar, COLOR_BAR_BG);

    drawPlayPause(canvas, mPlayPause, info.paused, mDrag == Hit::PlayPause);
    drawFullScreen(canvas, mFullScreen, info.fullScreen);

    float progressFraction = 0.0f;

    if (info.durationMs > 0) {
        progressFraction = clamp01(static_cast<float>(info.positionMs) / static_cast<float>(info.durationMs));
    }

    /* While dragging, show where the mouse is rather than where playback is. */
    if (mDrag == Hit::Progress) {
        progressFraction = mDragFraction;
    }

    drawSlider(canvas, mProgress, progressFraction, mDrag == Hit::Progress);

    /* Volume is 0.0 .. 1.0 (player_types.cpp default 1.0, clamped by
     * SuperMediaPlayer::SetVolume), which maps straight onto the track. */
    float volumeFraction = clamp01(info.volume);

    if (mDrag == Hit::Volume) {
        volumeFraction = mDragFraction;
    }

    drawSlider(canvas, mVolume, volumeFraction, mDrag == Hit::Volume);
}

void SdlControlBar::draw(Canvas &canvas, const Info &info)
{
    std::lock_guard<std::mutex> lock(mMutex);
    drawBar(canvas, info);
}

void SdlControlBar::draw(SDL_Renderer *renderer, const Info &info)
{
    if (renderer == nullptr) {
        return;
    }

    SdlRendererCanvas canvas(renderer);
    draw(canvas, info);
}

void SdlControlBar::drawToBitmap(uint32_t *pixels, const Info &info) const
{
    if (pixels == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);

    if (mBar.w <= 0 || mBar.h <= 0) {
        return;
    }

    /*
     * The bitmap covers the bar only, the drawing code works in window
     * coordinates: offset by -mBar.y so the bar lands at row 0.
     */
    BgraBitmapCanvas canvas(pixels, mBar.w, mBar.h, 0, mBar.y);
    drawBar(canvas, info);
}

int SdlControlBar::barWidth() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mBar.w;
}

int SdlControlBar::barHeight() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mBar.h;
}

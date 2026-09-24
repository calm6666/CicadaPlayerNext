//
// Mouse driven on-screen control bar for the cmdline demo.
//
// Windows only: the demo takes over presentation through
// MediaPlayer::SetVideoRenderingCallback(), draws the video and then this bar
// into the same SDL_Renderer, and presents. Doing it here keeps the framework
// untouched - SdlAFVideoRender is shared with the Linux and macOS builds and is
// deliberately not modified.
//
// There is no font library in the dependency set (only SDL2, no SDL2_ttf), so
// every control is drawn from rectangles: a triangle for play, two bars for
// pause, a circle for the sliders, corner brackets for full screen.
//

#ifndef CICADAMEDIA_SDLCONTROLBAR_H
#define CICADAMEDIA_SDLCONTROLBAR_H

#include <SDL2/SDL.h>
#include <cstdint>
#include <mutex>

class SdlControlBar {
public:
    /*
     * 绘图目标抽象。
     *
     * 控件条本身只用“填一个矩形”这一种图元（播放三角也是逐行矩形拼出来的），
     * 所以把这一种操作抽出来，SDL 呈现和零拷贝 D3D11 呈现（CPU 位图 +
     * CopySubresourceRegion）就能共用同一套外观代码，不会改一边忘一边。
     */
    class Canvas {
    public:
        virtual ~Canvas() = default;
        virtual void fill(const SDL_Rect &rect, const SDL_Color &color) = 0;
    };

    /* What the caller has to act on after a click or drag. */
    enum class Hit {
        None,
        PlayPause,
        Progress,
        Volume,
        FullScreen
    };

    /* Everything the bar needs to draw itself, read from the player each frame. */
    struct Info {
        int64_t positionMs = 0;
        int64_t durationMs = 0;
        float volume = 1.0f;
        bool paused = false;
        bool fullScreen = false;
    };

    SdlControlBar() = default;
    ~SdlControlBar() = default;

    SdlControlBar(const SdlControlBar &) = delete;
    SdlControlBar &operator=(const SdlControlBar &) = delete;

    /*
     * Recomputes the layout for a window of this size, in drawable pixels.
     * Cheap enough to call once per frame.
     */
    void layout(int windowWidth, int windowHeight);

    /*
     * Mouse handling. mouseDown returns the control that was grabbed, or
     * Hit::None when the click missed the bar. While a drag is in progress the
     * caller should keep applying dragFraction() - that is what makes the
     * progress and volume bars follow the mouse.
     *
     * A click on a hidden bar is swallowed: it only wakes the bar up and
     * returns Hit::None. Without that, the invisible bar keeps answering for
     * the bottom strip of the window, so clicking the picture there paused the
     * player or jumped the position with nothing on screen to explain it.
     */
    Hit mouseDown(int x, int y);
    void mouseMotion(int x, int y);
    void mouseUp();

    /*
     * The pointer left the window. Without this the hover flag stays set (SDL
     * only sends motion while the pointer is inside), the bar never hides and
     * keeps answering clicks in the bottom strip of the picture.
     */
    void mouseLeave();

    bool isDragging() const;

    /* Which control currently owns the drag, or Hit::None. */
    Hit dragControl() const;

    /* 0.0 .. 1.0 along the dragged track. Only meaningful for Progress/Volume. */
    float dragFraction() const;

    /* Fraction the given point maps to along a control's track, clamped. */
    float fractionAt(Hit control, int x) const;

    /* True when the point is inside the bar, so the caller can suppress the
     * double-click-fullscreen shortcut there. */
    bool contains(int x, int y) const;

    /* Keeps the bar on screen for a few seconds after any mouse activity. */
    void notifyActivity();
    bool visible() const;

    /* Draws into an SDL_Renderer (the classic path, no NV12 / no direct mode). */
    void draw(SDL_Renderer *renderer, const Info &info);

    /* Draws into any other target, e.g. the CPU bitmap used by -direct. */
    void draw(Canvas &canvas, const Info &info);

    /*
     * 零拷贝路径用：把控件条画成一张 BGRA 位图（尺寸 = barWidth() x
     * barHeight()，由 layout() 决定），调用方再用 CopySubresourceRegion 贴到
     * 后台缓冲底部。那条路径没有混合能力，所以这里把 alpha 与黑色预乘后写成
     * 不透明像素，和 SDL 上看到的深浅一致。
     */
    void drawToBitmap(uint32_t *pixels, const Info &info) const;

    /* 位图尺寸，等于最近一次 layout() 里的条宽度 / 条高度。 */
    int barWidth() const;
    int barHeight() const;

private:
    void drawBar(Canvas &canvas, const Info &info) const;
    void drawPlayPause(Canvas &canvas, const SDL_Rect &area, bool paused, bool highlight) const;
    void drawFullScreen(Canvas &canvas, const SDL_Rect &area, bool fullScreen) const;
    void drawSlider(Canvas &canvas, const SDL_Rect &track, float fraction, bool highlight) const;
    void drawTriangle(Canvas &canvas, const SDL_Rect &area, SDL_Color color) const;

    /* fractionAt() with mMutex already held, for the internal callers. */
    float fractionAtLocked(Hit control, int x) const;

    /* visible() with mMutex already held. */
    bool visibleLocked() const;

    SDL_Rect mBar{};
    SDL_Rect mPlayPause{};
    SDL_Rect mProgress{};
    SDL_Rect mVolume{};
    SDL_Rect mFullScreen{};

    Hit mDrag = Hit::None;
    float mDragFraction = 0.0f;
    bool mHovered = false;
    Uint32 mLastActivity = 0;

    /*
     * Input arrives on the main thread while draw() runs on the framework's
     * VSync thread, and the layout rectangles must not be read half-updated
     * (a torn SDL_Rect would make a button jump or a hit test miss).
     * The critical sections are tiny and never call back into the player.
     */
    mutable std::mutex mMutex;
};

#endif //CICADAMEDIA_SDLCONTROLBAR_H

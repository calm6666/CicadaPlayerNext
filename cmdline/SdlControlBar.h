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
     * progress and volume bars follow the mouse live.
     */
    Hit mouseDown(int x, int y);
    void mouseMotion(int x, int y);
    void mouseUp();

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

    void draw(SDL_Renderer *renderer, const Info &info);

private:
    void drawPlayPause(SDL_Renderer *renderer, const SDL_Rect &area, bool paused, bool highlight);
    void drawFullScreen(SDL_Renderer *renderer, const SDL_Rect &area, bool fullScreen);
    void drawSlider(SDL_Renderer *renderer, const SDL_Rect &track, float fraction, bool highlight);
    void drawTriangle(SDL_Renderer *renderer, const SDL_Rect &area, SDL_Color color);

    /* fractionAt() with mMutex already held, for the internal callers. */
    float fractionAtLocked(Hit control, int x) const;

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

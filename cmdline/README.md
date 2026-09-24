## command line tool cicadaPlayer usage

### 1. You can use keyboard to control

- **[Close]** button on the window

    to exit
- **[SPACE]**

    pause/play
- **[RIGHT]**

    seek forward
- **[LIFT]**

    seek back
- **[UP]**

    turn up volume
- **[DOWN]**

    turn down volume
- **[F7]**

    speed down speed of play back
- **[F9]**

    speed up speed of play back
### 2. You can send a command from network

see [tools](tools/command.sh)

### 3. Windows: presentation and frame rate

On Windows the demo presents the video itself (it takes over
`MediaPlayer::SetVideoRenderingCallback()`), and it recognises the mouse so the
control bar can be used. There are two presentation paths:

| | zero copy (`-direct`, default) | copy-back + SDL (`-nodirect`) |
|---|---|---|
| decode output | the raw D3D11 texture | downloaded to memory |
| presentation | own D3D11 swap chain + `ID3D11VideoProcessor` | `SDL_UpdateYUVTexture`/`SDL_UpdateNVTexture` |
| CPU copies per 4K frame | none | ~25MB |
| picture while the window is dragged / resized | keeps playing | freezes until the drag ends |

The zero-copy path is probed at startup (`D3d11DirectPresenter::supported()`);
when the GPU or driver cannot do NV12 video processing the demo stays on the
copy-back path automatically. `-nodirect` forces it, `-sw` (software decoding)
always uses it.

Frame rate: the render callback runs at the display refresh rate (queried at
startup, `video.render.hz`), and the swap chain is throttled by the display's
vertical blank through a waitable swap chain object. Frames are therefore shown
at the source frame rate - 24fps content is not resampled onto a 60Hz grid, so
there is no 3:2 judder on a 120Hz display - and never faster than the panel.

HDR: 10bit HDR10/HLG sources (P010 textures) are passed through untouched when
Windows has HDR switched on for that display (a `R10G10B10A2` + PQ/BT.2020 swap
chain), and tone mapped onto SDR by the video processor when it has not. The
display HDR state is re-checked every couple of seconds, so toggling HDR in
Windows Settings is picked up without restarting.

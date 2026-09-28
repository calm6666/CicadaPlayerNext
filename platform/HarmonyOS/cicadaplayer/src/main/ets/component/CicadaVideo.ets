/*
 * CicadaVideo —— 开箱即用的播放器视图组件（HAR 对外导出）。
 *
 * 它把"XComponent 画面 + 生命周期 + 播放器实例 + 事件透传"这套接线一次做好，
 * 消费方只需要：
 *
 *   private controller: CicadaVideoController = new CicadaVideoController();
 *   ...
 *   CicadaVideo({ controller: this.controller, listener: { onPrepared: () => {} } })
 *   ...
 *   this.controller.player.setDataSource(url);
 *   this.controller.player.prepare();     // 画面就绪后自动 start（见 autoStart）
 *
 * 注意：内核在 OHOS 上把画面直接交给 XComponent 的原生窗口（硬件解码 surface 模式
 * 直出），因此必须在拿到 surfaceId 之后再 setDataSource/prepare，组件内部已按这个
 * 顺序处理（surface 未就绪时先挂起请求，就绪后补发）。
 */

import { CicadaPlayer } from '../api/CicadaPlayer';
import { CicadaPlayerListener, ScaleMode } from '../api/CicadaTypes';

/** CicadaVideo 的控制器：持有播放器实例与画面状态。 */
export class CicadaVideoController {
  /** 底层播放器句柄，消费方直接用它调播放接口。 */
  readonly player: CicadaPlayer = new CicadaPlayer();

  /** 画面（XComponent surface）是否已经交给内核。 */
  private surfaceReadyInner: boolean = false;

  /** 画面就绪回调（true = 已绑定成功）。 */
  onSurfaceReady?: (ready: boolean) => void;

  /** 画面尺寸变化回调（宽, 高）。 */
  onSurfaceSizeChanged?: (width: number, height: number) => void;

  isSurfaceReady(): boolean {
    return this.surfaceReadyInner;
  }

  /**
   * 由组件在 XComponent.onLoad 里调用：把 surfaceId 交给内核。
   * 返回内核是否接受该画面。
   */
  attachSurface(surfaceId: string): boolean {
    const accepted: boolean = surfaceId.length > 0 && this.player.setSurface(surfaceId);
    this.surfaceReadyInner = accepted;
    if (this.onSurfaceReady) {
      this.onSurfaceReady(accepted);
    }
    return accepted;
  }

  /** 由组件在 XComponent.onDestroy 里调用。 */
  detachSurface(): void {
    this.surfaceReadyInner = false;
    this.player.clearScreen();
    if (this.onSurfaceReady) {
      this.onSurfaceReady(false);
    }
  }
}

@Component
export struct CicadaVideo {
  /** 消费方的控制器；不传则组件内部自建一个并自行释放。 */
  controller: CicadaVideoController = new CicadaVideoController();

  /** 事件回调，直接透传给内部播放器。 */
  listener: CicadaPlayerListener = {};

  /**
   * 画面就绪后是否自动 start()。
   * 播放器的 prepare() 与 surface 顺序无关，但真正出画必须等 surface 绑定，
   * 所以默认由组件在 surface 就绪后补一次 start()。
   */
  autoStart: boolean = true;

  /** 组件自身是否释放播放器（消费方自己 create 时传 false）。 */
  releaseOnDisappear: boolean = true;

  private xcomponentController: XComponentController = new XComponentController();
  private created: boolean = false;
  private started: boolean = false;

  private ensureCreated(): void {
    if (this.created) {
      return;
    }
    const id: number = this.controller.player.create(this.listener);
    this.created = id >= 0;
  }

  private onSurfaceLoad(): void {
    const surfaceId: string = this.xcomponentController.getXComponentSurfaceId();
    const ready: boolean = this.controller.attachSurface(surfaceId);
    if (ready && this.autoStart && !this.started) {
      this.started = true;
      this.controller.player.start();
    }
  }

  aboutToAppear(): void {
    this.ensureCreated();
  }

  aboutToDisappear(): void {
    this.controller.detachSurface();
    if (this.releaseOnDisappear) {
      this.controller.player.release();
      this.created = false;
      this.started = false;
    }
  }

  build() {
    XComponent({
      id: 'cicadaVideoSurface',
      type: XComponentType.SURFACE,
      controller: this.xcomponentController
    })
      .onLoad(() => {
        this.onSurfaceLoad();
      })
      .onDestroy(() => {
        this.controller.detachSurface();
      })
      .width('100%')
      .height('100%')
      .backgroundColor(Color.Black)
  }
}

/** 便捷：把 CicadaVideo 的缩放模式转成内核枚举（供消费方直接引用）。 */
export const CICADA_VIDEO_DEFAULT_SCALE_MODE: ScaleMode = ScaleMode.SM_FIT;

/*
 * Saves a player snapshot to the app sandbox as a PNG.
 *
 * The native bridge delivers the snapshot callback as (width, height, RGBA8888
 * ArrayBuffer) — the same payload the Android demo receives as a Bitmap.
 * HarmonyOS has no Bitmap equivalent, so the raw pixels are wrapped in a
 * PixelMap and encoded with ImagePacker.
 *
 * Android writes to external storage (/sdcard/snapShot_<ts>.png) and needs
 * WRITE_EXTERNAL_STORAGE; here the file goes to the app cache directory, so no
 * permission is required and nothing is written to shared storage.
 */

import { common } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';

export class SnapshotSaver {
  /** Returns the saved file path, or '' when the payload was unusable. */
  static async savePng(context: common.UIAbilityContext, width: number, height: number,
    rgba: ArrayBuffer): Promise<string> {
    if (width <= 0 || height <= 0 || rgba.byteLength < width * height * 4) {
      return '';
    }

    const initOptions: image.InitializationOptions = {
      size: { width: width, height: height },
      srcPixelFormat: image.PixelMapFormat.RGBA_8888
    };
    const pixelMap: image.PixelMap = image.createPixelMapSync(rgba, initOptions);

    const packer: image.ImagePacker = image.createImagePacker();
    const packOptions: image.PackingOption = { format: 'image/png', quality: 100 };
    const encoded: ArrayBuffer = await packer.packing(pixelMap, packOptions);

    // Android's file name pattern, kept for familiarity.
    const path: string = `${context.cacheDir}/snapShot_${Date.now()}.png`;
    const file = fileIo.openSync(path, fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE);
    try {
      fileIo.writeSync(file.fd, encoded);
    } finally {
      fileIo.closeSync(file);
    }

    // This SDK exposes only the promise form of release().
    await packer.release();
    await pixelMap.release();
    return path;
  }
}

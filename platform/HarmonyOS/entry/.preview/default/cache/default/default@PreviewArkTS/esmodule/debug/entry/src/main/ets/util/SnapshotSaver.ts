import type common from "@ohos:app.ability.common";
import fileIo from "@ohos:file.fs";
import image from "@ohos:multimedia.image";
export class SnapshotSaver {
    /** Returns the saved file path, or '' when the payload was unusable. */
    static async savePng(context: common.UIAbilityContext, width: number, height: number, rgba: ArrayBuffer): Promise<string> {
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
        }
        finally {
            fileIo.closeSync(file);
        }
        // This SDK exposes only the promise form of release().
        await packer.release();
        await pixelMap.release();
        return path;
    }
}

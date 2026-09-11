import fileIo from "@ohos:file.fs";
import fileUri from "@ohos:file.fileuri";
export class LocalVideoSource {
    /**
     * The descriptor backing the most recent /proc/self/fd/N result. One holder is
     * enough because only one local file is played at a time; it is replaced (and
     * closed) on the next resolve.
     */
    private static heldFile: fileIo.File | null = null;
    /**
     * Returns an openable path, or '' when neither strategy worked. Never copies.
     */
    static resolve(uri: string): string {
        if (uri.length === 0) {
            return '';
        }
        // ---- strategy 1: a plain filesystem path ----
        const candidates: string[] = [];
        if (uri.startsWith('/')) {
            candidates.push(uri);
        }
        else {
            try {
                const parsed = new fileUri.FileUri(uri);
                if (parsed.path !== undefined && parsed.path.length > 0) {
                    candidates.push(parsed.path);
                }
            }
            catch (err) {
                // Not a parseable URI; fall through to the fd strategy.
            }
            // "file:///abs/path" -> "/abs/path"
            if (uri.startsWith('file:///')) {
                candidates.push(uri.substring('file://'.length));
            }
        }
        for (const candidate of candidates) {
            if (LocalVideoSource.isReadable(candidate)) {
                return candidate;
            }
        }
        // ---- strategy 2: keep the fd and expose it through /proc ----
        try {
            LocalVideoSource.releaseHeld();
            const file: fileIo.File = fileIo.openSync(uri, fileIo.OpenMode.READ_ONLY);
            const fdPath: string = `/proc/self/fd/${file.fd}`;
            if (LocalVideoSource.isReadable(fdPath)) {
                LocalVideoSource.heldFile = file;
                return fdPath;
            }
            fileIo.closeSync(file);
        }
        catch (err) {
            // Unreadable URI.
        }
        return '';
    }
    /** Closes the descriptor held for the current /proc/self/fd/N path. */
    static releaseHeld(): void {
        const file = LocalVideoSource.heldFile;
        LocalVideoSource.heldFile = null;
        if (file === null) {
            return;
        }
        try {
            fileIo.closeSync(file);
        }
        catch (err) {
            // Best effort.
        }
    }
    /** True when the path exists and can be opened (used to rank candidates). */
    static isReadable(path: string): boolean {
        try {
            return fileIo.accessSync(path);
        }
        catch (err) {
            return false;
        }
    }
    /**
     * True only for a *file* URI (what the pickers hand back), as opposed to a
     * network URL or a plain filesystem path.
     *
     * This must NOT be a generic "looks like a URI" test: an http(s) URL also
     * contains "://", and treating one as a file made the AVPlayer page try to
     * open() it, which fails and surfaced as "open failed" for every network
     * source.
     */
    static isFileUri(value: string): boolean {
        return value.startsWith('file://');
    }
}

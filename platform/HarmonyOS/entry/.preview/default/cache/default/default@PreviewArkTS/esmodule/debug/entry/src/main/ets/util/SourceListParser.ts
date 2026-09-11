import type common from "@ohos:app.ability.common";
import util from "@ohos:util";
import type { SourceGroup, SourceSample } from '../model/CicadaTypes';
export class SourceListParser {
    static readonly ASSET_NAME: string = 'sourceList.json';
    /** Parses the bundled list. Returns [] when the asset is missing/invalid. */
    static parse(context: common.UIAbilityContext): SourceGroup[] {
        try {
            const bytes: Uint8Array = context.resourceManager.getRawFileContentSync(SourceListParser.ASSET_NAME);
            const decoder: util.TextDecoder = util.TextDecoder.create('utf-8');
            const json: string = decoder.decodeToString(bytes);
            const groups: SourceGroup[] = JSON.parse(json) as SourceGroup[];
            return groups !== null && groups !== undefined ? groups : [];
        }
        catch (err) {
            return [];
        }
    }
    /**
     * Unique group names in file order.
     * Mirrors SourceListParser.getDateTitleKey (dedup, JSON order preserved).
     */
    static groupNames(groups: SourceGroup[]): string[] {
        const names: string[] = [];
        for (const group of groups) {
            if (group.name !== undefined && !names.includes(group.name)) {
                names.push(group.name);
            }
        }
        return names;
    }
    /**
     * Samples of one group. Mirrors SourceListParser.handleDate, whose
     * URL_LINKS / HARDWARE_LINKS branches all do the same thing, so `name` is the
     * only grouping key.
     */
    static samplesOf(groups: SourceGroup[], groupName: string): SourceSample[] {
        for (const group of groups) {
            if (group.name === groupName) {
                return group.samples !== undefined ? group.samples : [];
            }
        }
        return [];
    }
    /**
     * Whether an entry is playable straight from the list. The Android demo
     * compares with equalsIgnoreCase, which is also why the local-file fallback's
     * "URL" type works.
     */
    static isPlayable(sample: SourceSample): boolean {
        const type: string = sample.type !== undefined ? sample.type.toUpperCase() : '';
        return type === 'URL' || type === 'LIVE';
    }
    /** True when the entry carries a usable external subtitle (cn/en only). */
    static hasExternalSubtitle(sample: SourceSample): boolean {
        const sub = sample.subtitle;
        if (sub === undefined || sub === null) {
            return false;
        }
        const cn: string = sub.cn !== undefined ? sub.cn : '';
        const en: string = sub.en !== undefined ? sub.en : '';
        return cn.length > 0 || en.length > 0;
    }
    /** Media file extensions accepted by the Android local-file fallback. */
    static readonly VIDEO_EXTENSIONS: string[] = ['.mp4', '.mp3', '.flv', '.m3u8', '.mov'];
    /** True when a file name looks like a playable local medium. */
    static isVideoFile(fileName: string): boolean {
        const lower: string = fileName.toLowerCase();
        for (const ext of SourceListParser.VIDEO_EXTENSIONS) {
            if (lower.endsWith(ext)) {
                return true;
            }
        }
        return false;
    }
}

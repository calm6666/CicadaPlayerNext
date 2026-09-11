import type common from "@ohos:app.ability.common";
import preferences from "@ohos:data.preferences";
const STORE_NAME: string = 'cicada_player';
const KEY_HARDWARE_DECODER: string = 'cicada_player_hardware_decoder';
const KEY_SELECTED_PLAYER: string = 'selected_player_type';
export const PLAYER_TYPE_CICADA: string = 'CicadaPlayer';
/**
 * HarmonyOS system player. Fills the Android demo's "MediaPlayer" kernel slot;
 * the Android "ExoPlayer" kernel has no counterpart and is not ported.
 */
export const PLAYER_TYPE_AVPLAYER: string = 'AVPlayer';
/**
 * AppStorage key holding the last created player's name. The C API only exposes
 * CicadaGetPlayerName(handle) -- there is no global version call -- so the player
 * page caches the name here and the settings page reads it instead of building a
 * throwaway player just to display it.
 */
export const PLAYER_NAME_KEY: string = 'playerName';
export class Settings {
    private static store: preferences.Preferences | null = null;
    /** Android's default for this flag is true. */
    private static hardwareDecoder: boolean = true;
    private static playerType: string = PLAYER_TYPE_CICADA;
    /** Loads the persisted values; safe to call more than once. */
    static async init(context: common.UIAbilityContext): Promise<void> {
        if (Settings.store !== null) {
            return;
        }
        try {
            const options: preferences.Options = { name: STORE_NAME };
            Settings.store = await preferences.getPreferences(context, options);
            const hw: preferences.ValueType = await Settings.store.get(KEY_HARDWARE_DECODER, true);
            Settings.hardwareDecoder = hw as boolean;
            const pt: preferences.ValueType = await Settings.store.get(KEY_SELECTED_PLAYER, PLAYER_TYPE_CICADA);
            Settings.playerType = pt as string;
        }
        catch (err) {
            // Keep the in-memory defaults; the demo still works without persistence.
            Settings.store = null;
        }
    }
    static isHardwareDecoderEnabled(): boolean {
        return Settings.hardwareDecoder;
    }
    static async setHardwareDecoder(enabled: boolean): Promise<void> {
        Settings.hardwareDecoder = enabled;
        await Settings.persist(KEY_HARDWARE_DECODER, enabled);
    }
    static getPlayerType(): string {
        return Settings.playerType;
    }
    static async setPlayerType(playerType: string): Promise<void> {
        Settings.playerType = playerType;
        await Settings.persist(KEY_SELECTED_PLAYER, playerType);
    }
    private static async persist(key: string, value: preferences.ValueType): Promise<void> {
        if (Settings.store === null) {
            return;
        }
        try {
            await Settings.store.put(key, value);
            await Settings.store.flush();
        }
        catch (err) {
            // Ignore: persistence is best-effort.
        }
    }
}

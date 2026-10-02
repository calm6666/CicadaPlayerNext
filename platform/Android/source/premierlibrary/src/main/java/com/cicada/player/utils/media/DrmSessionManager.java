package com.cicada.player.utils.media;

import android.annotation.SuppressLint;
import android.media.MediaDrm;
import android.media.NotProvisionedException;
import android.media.UnsupportedSchemeException;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Message;
import android.util.Base64;

import com.cicada.player.utils.Logger;
import com.cicada.player.utils.NativeUsed;

import java.util.Locale;
import java.util.UUID;

import static android.media.MediaDrm.EVENT_KEY_REQUIRED;
import static android.media.MediaDrm.EVENT_PROVISION_REQUIRED;

/**
 * Android MediaDrm 会话管理（由 native 的 DrmHandlerPrototype 按需创建）。
 *
 * <p>【支持哪几套 DRM】这里原来只认 Widevine 一条硬编码的 UUID，于是"清单里声明的是
 * PlayReady / ClearKey"时 {@code prepare()} 直接报 {@code not support format} 并置错误态 ——
 * 也就是说 Android 上除 Widevine 以外的 DRM 一条都播不了。现在按 keyFormat（DRM 系统标识）
 * 选 UUID，覆盖 Android MediaDrm 真正能建会话的三家：
 *
 * <ul>
 *   <li>Widevine {@code urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed}
 *   <li>PlayReady {@code urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95}
 *   <li>ClearKey（DASH-IF / W3C）{@code urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e}
 * </ul>
 *
 * <p>候选集合是确定的，但**设备是否真的支持**由 {@link MediaDrm#isCryptoSchemeSupported(UUID)}
 * 决定：不支持就在 {@code prepare()} 里明确置错误态（{@code ERROR_CODE_UNSUPPORT_SCHEME}），
 * 不静默降级。
 *
 * <p>【initData 从哪来】解密/取密钥要的是 **PSSH**。manifest 里的 {@code <cenc:pssh>} 由
 * native 侧解析后经 {@link #requireSession} 传进来（原来根本没传，只传了一个 url，
 * 于是 {@code requestKey()} 只能从一个"url,base64"的数据 URI 里硬切 —— 那是历史遗留写法）。
 * 清单只写了 {@code cenc:default_KID} 而没有 pssh 时，ClearKey 这条按 W3C EME 的
 * "Common PSSH box" 格式**按规范构造**一个（见 {@link #buildCommonPssh}），
 * 而不是猜。
 */
@NativeUsed
public class DrmSessionManager {

    private static final String TAG = DrmSessionManager.class.getSimpleName();

    /** Widevine（W3C EME key system: com.widevine.alpha）。 */
    private static final String WIDEVINE_FORMAT = "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
    public static final UUID WIDEVINE_UUID = new UUID(0xEDEF8BA979D64ACEL, 0xA3C827DCD51D21EDL);

    /** Microsoft PlayReady。 */
    private static final String PLAYREADY_FORMAT = "urn:uuid:9a04f079-9840-4286-ab92-e65be0885f95";
    private static final UUID PLAYREADY_UUID = new UUID(0x9A04F07998404286L, 0xAB92E65BE0885F95L);

    /** ClearKey（DASH-IF 登记表里的 Clear Key / W3C ClearKey）。 */
    private static final String CLEARKEY_FORMAT = "urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e";
    private static final UUID CLEARKEY_UUID = new UUID(0xE2719D58A985B3C9L, 0x781AB030AF78D30EL);

    /**
     * W3C "Common PSSH box" 的 SystemID。
     *
     * <p>来源：W3C EME 初始化数据格式登记表（cenc.html）—— 一个固定 UUID
     * {@code 1077efec-c0b2-4d02-ace3-3c1e52e2fb4b}，box 负载是 KID 列表。
     * 只有要把"清单里只有 default_KID"变成"MediaDrm 能吃的 initData"时才会用到它。
     */
    private static final String COMMON_PSSH_SYSTEM_ID = "1077efec-c0b2-4d02-ace3-3c1e52e2fb4b";

    private long mNativeInstance = 0;

    public DrmSessionManager(long nativeInstance) {
        mNativeInstance = nativeInstance;
    }

    private DrmSession drmSession = null;

    public static int SESSION_STATE_ERROR = -1;
    public static int SESSION_STATE_IDLE = -2;
    public static int SESSION_STATE_OPENED = 0;

    public static int ERROR_CODE_NONE = 0;
    public static int ERROR_CODE_UNSUPPORT_SCHEME = 1;
    public static int ERROR_CODE_RESOURCE_BUSY = 2;
    public static int ERROR_CODE_KEY_RESPONSE_NULL = 3;
    public static int ERROR_CODE_PROVISION_RESPONSE_NULL = 4;
    public static int ERROR_CODE_DENIED_BY_SERVER = 5;
    public static int ERROR_CODE_RELEASED = 6;
    public static int ERROR_CODE_PROVISION_FAIL = 7;

    private static class DrmInfo {
        public String licenseUrl = null;
        public String keyUrl = null;
        public String keyFormat = null;
        public String mime = null;
        /** base64 的 PSSH box（manifest 的 <cenc:pssh>），没有就是 null。 */
        public String pssh = null;
        /** cenc:default_KID（UUID 文本或 32 hex），没有就是 null。 */
        public String keyId = null;

        public boolean isSame(DrmInfo info) {
            if (info == null) {
                return false;
            }

            if (!areEqual(keyUrl, info.keyUrl)) {
                return false;
            }

            if (!areEqual(licenseUrl, info.licenseUrl)) {
                return false;
            }

            if (!areEqual(keyFormat, info.keyFormat)) {
                return false;
            }

            if (!areEqual(pssh, info.pssh)) {
                return false;
            }

            return areEqual(keyId, info.keyId);
        }

        private static boolean areEqual(Object o1, Object o2) {
            return o1 == null ? o2 == null : o1.equals(o2);
        }
    }

    /**
     * 把清单/native 给的 DRM 系统标识归一化成 {@code urn:uuid:<小写>}。
     *
     * <p>清单里同一套 DRM 有好几种写法（大写 UUID、裸 UUID、带花括号、EME 的
     * {@code com.widevine.alpha} 等等），字符串直比必然漏。这里只做"归一化"，
     * 不做"猜测"：认不出来就返回 null，由调用方明确报"不支持"。
     */
    private static String canonicalFormat(String format) {
        if (format == null) {
            return null;
        }

        String s = format.trim().toLowerCase(Locale.US);

        if (s.isEmpty()) {
            return null;
        }

        // EME / HLS KEYFORMAT 的惯用别名（只映射到本文件里真正有 UUID 的三家）
        if ("com.widevine.alpha".equals(s)) {
            return WIDEVINE_FORMAT;
        }

        if ("com.microsoft.playready".equals(s)
                || "com.microsoft.playready.recommendation".equals(s)) {
            return PLAYREADY_FORMAT;
        }

        if ("org.w3.clearkey".equals(s)) {
            return CLEARKEY_FORMAT;
        }

        if (s.startsWith("urn:uuid:")) {
            String body = s.substring("urn:uuid:".length());
            return "urn:uuid:" + stripBraces(body);
        }

        String bare = stripBraces(s);

        if (bare.length() == 36) {
            return "urn:uuid:" + bare;
        }

        if (bare.length() == 32 && isHex(bare)) {
            return "urn:uuid:" + insertDashes(bare);
        }

        return null;
    }

    private static String stripBraces(String s) {
        if (s.length() >= 2 && s.charAt(0) == '{' && s.charAt(s.length() - 1) == '}') {
            return s.substring(1, s.length() - 1);
        }

        return s;
    }

    private static boolean isHex(String s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);

            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }

        return true;
    }

    private static String insertDashes(String hex32) {
        return hex32.substring(0, 8) + "-" + hex32.substring(8, 12) + "-"
                + hex32.substring(12, 16) + "-" + hex32.substring(16, 20) + "-"
                + hex32.substring(20, 32);
    }

    /** DRM 系统标识 → MediaDrm 的 scheme UUID；不是本文件认得的三家就返回 null。 */
    private static UUID uuidForFormat(String format) {
        String canonical = canonicalFormat(format);

        if (WIDEVINE_FORMAT.equals(canonical)) {
            return WIDEVINE_UUID;
        }

        if (PLAYREADY_FORMAT.equals(canonical)) {
            return PLAYREADY_UUID;
        }

        if (CLEARKEY_FORMAT.equals(canonical)) {
            return CLEARKEY_UUID;
        }

        return null;
    }

    /** "edef8ba9-79d6-…" → 16 字节；形状不对返回 null。 */
    private static byte[] uuidTextToBytes(String text) {
        if (text == null) {
            return null;
        }

        String hex = text.trim().toLowerCase(Locale.US).replace("-", "");
        hex = stripBraces(hex);

        if (hex.length() != 32 || !isHex(hex)) {
            return null;
        }

        byte[] out = new byte[16];

        for (int i = 0; i < 16; i++) {
            out[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
        }

        return out;
    }

    /**
     * 按 W3C EME 的 "Common PSSH box" 格式构造一个只含一个 KID 的 pssh box。
     *
     * <p>布局（来自 EME initdata 登记表的 cenc.html，字段顺序与位宽都是规范定死的）：
     * <pre>
     *   size(4) 'pssh'(4) version=1(1) flags=0(3)
     *   system_id[16]        = 1077efec-c0b2-4d02-ace3-3c1e52e2fb4b
     *   kid_count(4)         = 1
     *   key_id[16]
     * </pre>
     * 一个 KID 时总长 48 字节。
     *
     * <p>【为什么敢"构造"】这不是发明格式：这就是 EME 规定的"用 KID 表达 initData"的标准箱子，
     * Android 的 ClearKey 插件要的正是它。清单里既然已经给了 {@code cenc:default_KID}，
     * 用它拼出这个箱子是**按规范翻译**，不是猜。
     */
    private static byte[] buildCommonPssh(String keyIdText) {
        byte[] systemId = uuidTextToBytes(COMMON_PSSH_SYSTEM_ID);
        byte[] kid = uuidTextToBytes(keyIdText);

        if (systemId == null || kid == null) {
            return null;
        }

        final int boxSize = 8 + 4 + 16 + 4 + 16;
        byte[] box = new byte[boxSize];
        int p = 0;

        box[p++] = (byte) ((boxSize >> 24) & 0xFF);
        box[p++] = (byte) ((boxSize >> 16) & 0xFF);
        box[p++] = (byte) ((boxSize >> 8) & 0xFF);
        box[p++] = (byte) (boxSize & 0xFF);

        box[p++] = 'p';
        box[p++] = 's';
        box[p++] = 's';
        box[p++] = 'h';

        box[p++] = 1;   // version = 1（Common PSSH 只有 version 1 有 KID 列表）
        box[p++] = 0;   // flags = 0
        box[p++] = 0;
        box[p++] = 0;

        System.arraycopy(systemId, 0, box, p, 16);
        p += 16;

        box[p++] = 0;
        box[p++] = 0;
        box[p++] = 0;
        box[p++] = 1;   // kid_count = 1

        System.arraycopy(kid, 0, box, p, 16);

        return box;
    }

    private class DrmSession {
        public DrmInfo drmInfo = null;
        public MediaDrm mediaDrm = null;
        public byte[] sessionId = null;

        public int state = SESSION_STATE_IDLE;

        private HandlerThread requestHandlerThread = null;
        public Handler requestHandler = null;

        public DrmSession(DrmInfo info) {
            drmInfo = info;

            requestHandlerThread = new HandlerThread("DrmRequestHanderThread");
            requestHandlerThread.start();
            requestHandler = new Handler(requestHandlerThread.getLooper()) {
                @Override
                public void handleMessage(Message msg) {
                    int event = msg.what;
//                    Logger.v(TAG, "  handleMessage event = " + event);
                    if (event == EVENT_PROVISION_REQUIRED) {
                        requestProvision();
                    } else if (event == EVENT_KEY_REQUIRED
                            || event == MediaDrm.EVENT_KEY_EXPIRED) {
                        try {
                            requestKey();
                        } catch (NotProvisionedException e) {
                            requestProvision();
                        }
                    }

                    super.handleMessage(msg);
                }
            };
        }

        public boolean prepare(boolean allowProvisioning) {
            if (mediaDrm == null) {
                UUID scheme = uuidForFormat(drmInfo.keyFormat);

                if (scheme == null) {
                    Logger.e(TAG, " prepare fail : not support format :" + drmInfo.keyFormat);
                    changeState(SESSION_STATE_ERROR, ERROR_CODE_UNSUPPORT_SCHEME);
                    return false;
                }

                // 候选集合归一到三家了，但设备到底有没有这个 scheme 的插件由这里说了算。
                if (!MediaDrm.isCryptoSchemeSupported(scheme)) {
                    Logger.e(TAG, " prepare fail : this device has no " + drmInfo.keyFormat
                            + " plugin (MediaDrm.isCryptoSchemeSupported = false)");
                    changeState(SESSION_STATE_ERROR, ERROR_CODE_UNSUPPORT_SCHEME);
                    return false;
                }

                try {
                    mediaDrm = new MediaDrm(scheme);
                } catch (UnsupportedSchemeException e) {
                    Logger.e(TAG, " prepare fail : " + e.getMessage());
                    changeState(SESSION_STATE_ERROR, ERROR_CODE_UNSUPPORT_SCHEME);
                    return false;
                }

                mediaDrm.setOnEventListener(new MediaDrm.OnEventListener() {
                    @Override
                    public void onEvent(MediaDrm md, byte[] sessionId, int event, int extra, byte[] data) {
                        Logger.d(TAG, " drm Event = " + event + " , extra = " + extra + " , sessionId =  " + sessionId);
                        sendRequest(event, sessionId);
                    }
                });
            }

            try {
                sessionId = mediaDrm.openSession();
                native_updateSessionId(mNativeInstance, sessionId);
                changeState(SESSION_STATE_IDLE, ERROR_CODE_NONE);
                sendRequest(EVENT_KEY_REQUIRED, sessionId);
            } catch (NotProvisionedException e) {
                Logger.e(TAG, " prepare NotProvisionedException : " + e.getMessage());
                if (allowProvisioning) {
                    sendRequest(EVENT_PROVISION_REQUIRED, null);
                } else {
                    changeState(SESSION_STATE_ERROR, ERROR_CODE_PROVISION_FAIL);
                }
                return false;
            } catch (Exception e) {
                Logger.e(TAG, " prepare fail : " + e.getMessage());
                changeState(SESSION_STATE_ERROR, ERROR_CODE_RESOURCE_BUSY);
                return false;
            }

            return true;
        }

        private void sendRequest(int event, byte[] sessionId) {
            Message msg = requestHandler.obtainMessage(event, sessionId);
            requestHandler.sendMessage(msg);
        }

        public boolean release() {

            changeState(SESSION_STATE_ERROR, ERROR_CODE_RELEASED);

            requestHandlerThread.quit();

            if (mediaDrm != null) {
                try {
                    if (sessionId != null) {
                        mediaDrm.closeSession(sessionId);
                    }
                } catch (Exception e) {
                    Logger.e(TAG, " closeSession fail : " + e.getMessage());
                }
                try {
                    mediaDrm.release();
                } catch (Exception e) {
                    Logger.e(TAG, " release fail : " + e.getMessage());
                }
                mediaDrm = null;
            }

            return true;
        }

        /**
         * 组装 getKeyRequest 需要的 initData（PSSH）。
         *
         * <p>三种来源，按优先级：
         * <ol>
         *   <li>{@code drmInfo.pssh} —— manifest 里的 {@code <cenc:pssh>}，最权威；
         *   <li>{@code keyId} + ClearKey ⇒ 按 W3C Common PSSH box 规范构造（见
         *       {@link #buildCommonPssh}）；
         *   <li>历史遗留：{@code keyUrl} 是 {@code "url,<base64 pssh>"} 的数据 URI 写法。
         *       ★原来的代码是 {@code substring(indexOf(','))} —— 连逗号一起切进去了，
         *       解出来必然不是合法 box；这里改成 {@code indexOf(',') + 1}。
         * </ol>
         * 一个都拿不到就返回 null，由调用方按"取密钥失败"处理（不会把垃圾当 initData 用）。
         */
        private byte[] buildInitData() {
            if (drmInfo.pssh != null && !drmInfo.pssh.isEmpty()) {
                try {
                    return Base64.decode(drmInfo.pssh, Base64.DEFAULT);
                } catch (Exception e) {
                    Logger.e(TAG, " the declared cenc:pssh is not valid base64 : " + e.getMessage());
                }
            }

            if (CLEARKEY_FORMAT.equals(canonicalFormat(drmInfo.keyFormat))
                    && drmInfo.keyId != null && !drmInfo.keyId.isEmpty()) {
                byte[] common = buildCommonPssh(drmInfo.keyId);

                if (common != null) {
                    Logger.d(TAG, " built a W3C Common PSSH box from cenc:default_KID for ClearKey");
                    return common;
                }

                Logger.e(TAG, " cenc:default_KID is not a UUID : " + drmInfo.keyId);
            }

            if (drmInfo.keyUrl != null) {
                int comma = drmInfo.keyUrl.indexOf(',');

                if (comma >= 0) {
                    try {
                        return Base64.decode(drmInfo.keyUrl.substring(comma + 1), Base64.DEFAULT);
                    } catch (Exception e) {
                        Logger.e(TAG, " the legacy data-URI keyUrl carries no valid base64 : "
                                + e.getMessage());
                    }
                }
            }

            return null;
        }

        private void requestKey() throws NotProvisionedException {
            Logger.d(TAG, "requestKey state = " + state);
            if (state == SESSION_STATE_ERROR) {
                return;
            }

            try {
                byte[] initData = buildInitData();

                if (initData == null) {
                    Logger.e(TAG, " requestKey fail : no PSSH or key id is available "
                            + "(keyFormat=" + drmInfo.keyFormat + ")");
                    changeState(SESSION_STATE_ERROR, ERROR_CODE_KEY_RESPONSE_NULL);
                    return;
                }

                MediaDrm.KeyRequest keyRequest = mediaDrm.getKeyRequest(sessionId, initData,
                        drmInfo.mime, MediaDrm.KEY_TYPE_STREAMING, null);
                byte[] requestData = native_requestKey(mNativeInstance, keyRequest.getDefaultUrl(), keyRequest.getData());

                Logger.v(TAG, "requestKey result = " + new String(requestData));

                if (requestData == null) {
                    Logger.e(TAG, "requestKey fail: data = null , url : " + keyRequest.getDefaultUrl());
                    changeState(SESSION_STATE_ERROR, ERROR_CODE_KEY_RESPONSE_NULL);
                    return;
                }

                mediaDrm.provideKeyResponse(sessionId, requestData);
                changeState(SESSION_STATE_OPENED, ERROR_CODE_NONE);
            } catch (Exception e) {
                Logger.e(TAG, "requestKey fail: " + e.getMessage());
                changeState(SESSION_STATE_ERROR, ERROR_CODE_DENIED_BY_SERVER);
            }
        }

        private boolean hasProvideProvision = false;

        private void requestProvision() {

            Logger.d(TAG, "requestProvision  state = " + state);
            if (hasProvideProvision) {
                return;
            }

            MediaDrm.ProvisionRequest request = mediaDrm.getProvisionRequest();
            byte[] provisionData = native_requestProvision(mNativeInstance, request.getDefaultUrl(), request.getData());
            if (provisionData == null) {
                Logger.e(TAG, "requestProvision fail: data = null , url : " + request.getDefaultUrl());
                changeState(SESSION_STATE_ERROR, ERROR_CODE_PROVISION_RESPONSE_NULL);
                return;
            }

            Logger.d(TAG, "requestProvision : data =  " + new String(provisionData));

            try {
                mediaDrm.provideProvisionResponse(provisionData);
                hasProvideProvision = true;
                if (state == SESSION_STATE_IDLE) {
                    prepare(false);
                }
            } catch (Exception e) {
                Logger.e(TAG, "requestProvision fail: " + e.getMessage());
                changeState(SESSION_STATE_ERROR, ERROR_CODE_PROVISION_FAIL);
            }
        }

        private void changeState(int state, int errorCode) {
            this.state = state;
            Logger.d(TAG, "changeState " + state);
            native_changeState(mNativeInstance, state, errorCode);
        }

        @SuppressLint("WrongConstant")
        public boolean isForceInsecureDecoder() {
            if (mediaDrm != null) {
                return Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP &&
                        "L3".equals(mediaDrm.getPropertyString("securityLevel"));
            } else {
                return false;
            }
        }
    }

    /**
     * native 侧要求在 keyFormat 所指的 DRM 系统上开一个会话。
     *
     * <p>★参数顺序/个数与 native 是**一处约定**：{@code WideVineDrmHandler.cpp} 里
     * {@code jMediaDrmSession_requireSession} 的方法签名必须与这里逐字一致
     * （{@code (Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V}），
     * 改这里就一定要改那里。原来的签名只有两个 String，于是 manifest 里解析出来的
     * pssh / default_KID 根本传不到这一层 —— ClearKey 与"只有 pssh 没有 laurl"的清单
     * 都因此取不到密钥。
     */
    @NativeUsed
    public synchronized void requireSession(String keyFormat, String licenseUrl, String pssh,
                                            String keyId, String mime) {
        Logger.d(TAG, "requireSession info = " + keyFormat);

        DrmInfo info = new DrmInfo();
        info.keyFormat = keyFormat;
        info.licenseUrl = licenseUrl;
        // keyUrl 仍然留着：历史形态（"url,base64 pssh" 数据 URI）与 native 的
        // DrmInfo.uri 是同一个字段，两条兼容路径都读它。
        info.keyUrl = licenseUrl;
        info.pssh = pssh;
        info.keyId = keyId;
        info.mime = mime;

        requireSessionInner(info);
    }

    private void requireSessionInner(DrmInfo info) {
        if (drmSession == null) {
            drmSession = new DrmSession(info);
            drmSession.prepare(true);
        }
    }

    @NativeUsed
    public synchronized void releaseSession() {
        Logger.d(TAG, "releaseSession");
        if (drmSession != null) {
            drmSession.release();
            drmSession = null;
        }
    }

    @NativeUsed
    public boolean isForceInsecureDecoder() {
        if (drmSession != null) {
            return drmSession.isForceInsecureDecoder();
        } else {
            return false;
        }
    }

    @SuppressLint("ObsoleteSdkInt")
    @NativeUsed
    public static boolean supportDrm(String format) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.JELLY_BEAN_MR2) {
            return false;
        }

        UUID scheme = uuidForFormat(format);

        // 认不出来就是我们没有这个 scheme 的会话能力：明确 false，不"默认支持"。
        // 原来的写法是"只有 Widevine 需要查设备，其余一律 true" ——
        // 于是任何一个陌生 keyFormat 都会被当成支持，然后在 prepare() 里才失败。
        return scheme != null && MediaDrm.isCryptoSchemeSupported(scheme);
    }

    protected native byte[] native_requestProvision(long nativeInstance, String url, byte[] data);

    protected native byte[] native_requestKey(long nativeInstance, String url, byte[] data);

    protected native void native_changeState(long nativeInstance, int state, int errorCode);

    protected native void native_updateSessionId(long nativeInstance, byte[] sessionId);
}

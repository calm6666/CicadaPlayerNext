package com.cicada.player.demo;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.support.annotation.NonNull;
import android.support.v4.app.Fragment;
import android.support.v4.app.FragmentManager;
import android.support.v4.app.FragmentTransaction;
import android.text.TextUtils;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;

import com.google.android.exoplayer2.ui.AspectRatioFrameLayout;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.Toast;

import com.cicada.player.CicadaPlayer;
import com.cicada.player.bean.ErrorInfo;
import com.cicada.player.bean.InfoBean;
import com.cicada.player.bean.InfoCode;
import com.cicada.player.demo.bean.PlayerMediaInfo;
import com.cicada.player.demo.fragment.BaseFragment;
import com.cicada.player.demo.fragment.PlayerCacheConfigFragment;
import com.cicada.player.demo.fragment.PlayerConfigFragment;
import com.cicada.player.demo.fragment.PlayerOperationFragment;
import com.cicada.player.demo.fragment.PlayerTrackFragment;
import com.cicada.player.demo.util.FileUtils;
import com.cicada.player.demo.util.PerfMonitorUtils;
import com.cicada.player.demo.util.PermissionUtils;
import com.cicada.player.demo.util.ScreenStatusController;
import com.cicada.player.demo.util.ScreenUtils;
import com.cicada.player.demo.util.ThreadUtils;
import com.cicada.player.demo.view.CicadaVodPlayerView;
import com.cicada.player.nativeclass.MediaInfo;
import com.cicada.player.nativeclass.PlayerConfig;
import com.cicada.player.nativeclass.TrackInfo;
import com.cicada.player.demo.util.VcPlayerLog;

import java.io.File;
import java.util.LinkedHashMap;
import java.util.Locale;

/**
 * 视频播放界面
 */
public class CicadaPlayerActivity extends BaseActivity implements View.OnClickListener{

    /**
     * 权限请求码
     */
    private static final int PERMISSION_REQUEST_CODE = 1002;

    /**
     * url
     */
    private static final String DATA_SOURCE_URL = "data_source_url";
    /**
     * play_type
     */
    public static final String URL_TYPE = "url";
    public static final String LIVE_TYPE = "live";
    public static final String LOCAL_TYPE = "local";
    /**
     * 对象播放：接口返回的 MediaManifest JSON（反序列化后直接传入播放器）
     */
    public static final String MANIFEST_TYPE = "manifest";
    public static final String PLAY_TYPE = "play_type";
    public static final String SUBTITLE_EXT = "subtitle_ext";

    /**
     * 任务栈name
     */
    private static final String FRAGMENT_BACK_STACK_NAME = "player_back_stack";
    /**
     * 播放View
     */
    private CicadaVodPlayerView mCicadaVodPlayerView;

    /**
     * 播放性能调试悬浮层：解码方式 / App CPU / 系统 CPU / GPU 占用，每秒刷新
     */
    private FrameLayout mVideoContainer;
    /**
     * 视频等比容器（ExoPlayer 官方 AspectRatioFrameLayout，RESIZE_MODE_FIT）：
     * 按视频宽高比测量，不同比例/全屏自动等比黑边，永不拉伸
     */
    private AspectRatioFrameLayout mVideoAspectContainer;
    private TextView mPerfDebugTextView;
    private final Handler mPerfHandler = new Handler(Looper.getMainLooper());
    private Runnable mPerfRunnable;
    private boolean mUseSoftwareDecoder = false;
    private String mHwVideoCodecName = null;
    private long[] mLastProcCpu = null;
    private long[] mLastSysCpu = null;
    private long mLastPerfTickMs = 0;

    /**
     * 视频真实宽高（用于隧道渲染下等比调整容器）
     */
    private int mVideoWidth = 0;
    private int mVideoHeight = 0;

    /**
     * 全屏模式标记（由 CicadaVodPlayerView 的全屏按钮回调维护；
     * 竖拍视频全屏保持竖屏，需隐藏按钮/标题等外围 UI）
     */
    private boolean mFullScreenMode = false;
    private LinearLayout mBtnGroup;
    private View mFragmentContainer;
    /**
     * 当前选中的Fragment
     */
    private Fragment mCurrentFragment = new PlayerOperationFragment();
    /**
     * 准备
     */
    private Button mPrepareButton;
    /**
     * 播放
     */
    private Button mPlayButton;
    /**
     * 停止
     */
    private Button mStopButton;
    /**
     * 暂停
     */
    private Button mPauseButton;
    /**
     * 重试
     */
    private Button mRetryButton;
    /**
     * 播放方式
     */
    private String playType;
    /**
     * 截图
     */
    private Button mSnapShotButton;
    /**
     * url数据源
     */
    private String urlDataSource;
    /**
     * 要切换到的Fragment
     */
    private BaseFragment toFragment;

    /**
     * 是否正在截图中
     */
    private boolean inSnapShotting;
    private ScreenStatusController mScreenStatusController;
    private RelativeLayout rlTitleRoot;
    private RadioGroup mPlayerRaioGroup;

    /**
     * 外挂字幕链接地址
     */
    private PlayerMediaInfo.TypeInfo.SubtitleInfo mSubtitleInfos;
    private LinkedHashMap<Integer,String> mSubtitleMap = new LinkedHashMap<>();

    /**
     * 最后显示的外挂字幕
     */
    private int mLastTrackIndex;

    private int mRetryCount = 1;

    private boolean mEnablePlayBack = false;
    /**
     * url播放
     * @param context  context
     * @param url      url
     */
    public static void startApsaraPlayerActivityByUrl(Context context,String url){
        Intent intent = new Intent(context, CicadaPlayerActivity.class);
        intent.putExtra(DATA_SOURCE_URL,url);
        intent.putExtra(PLAY_TYPE,URL_TYPE);
        context.startActivity(intent);
    }

    /**
     * 对象播放（DASH/HLS）：接口返回的 MediaManifest JSON 字符串，
     * 播放器内反序列化后走对象传入路径
     * @param context context
     * @param mediaManifestJson MediaManifest JSON
     */
    public static void startApsaraPlayerActivityByManifestJson(Context context, String mediaManifestJson) {
        Intent intent = new Intent(context, CicadaPlayerActivity.class);
        intent.putExtra(DATA_SOURCE_URL, mediaManifestJson);
        intent.putExtra(PLAY_TYPE, MANIFEST_TYPE);
        context.startActivity(intent);
    }

    /**
     * 包含外挂字幕
     */
    public static void startApsaraPlayerActivityByUrlWithSubtitle(Context context, PlayerMediaInfo.TypeInfo typeInfo) {
        Intent intent = new Intent(context,CicadaPlayerActivity.class);
        intent.putExtra(DATA_SOURCE_URL,typeInfo.getUrl());
        intent.putExtra(PLAY_TYPE,URL_TYPE);
        intent.putExtra(SUBTITLE_EXT,typeInfo.getSubtitle());
        context.startActivity(intent);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_cicada_player);

        initIntent();
        initView();
        initListener();
    }

    private void initIntent(){
        Intent intent = getIntent();
        playType = intent.getStringExtra(PLAY_TYPE);
        mSubtitleInfos = (PlayerMediaInfo.TypeInfo.SubtitleInfo) intent.getSerializableExtra(SUBTITLE_EXT);
        if(TextUtils.isEmpty(playType)){
            playType = URL_TYPE;
        }
        else{
            //url
            if(TextUtils.isEmpty(DATA_SOURCE_URL)){
                urlDataSource = getIntent().getStringExtra("");
            }else{
                urlDataSource = getIntent().getStringExtra(DATA_SOURCE_URL);
            }
        }
    }


    private void initView() {
        mPlayButton = findViewById(R.id.play);
        mStopButton = findViewById(R.id.stop);
        mRetryButton = findViewById(R.id.retry);
        mPauseButton = findViewById(R.id.pause);
        mPrepareButton = findViewById(R.id.prepare);
        mSnapShotButton = findViewById(R.id.snapshot);

        TextView tvTitle = findViewById(R.id.tv_title);
        rlTitleRoot = findViewById(R.id.rl_title_root);
        mPlayerRaioGroup = findViewById(R.id.radio_group_player);
        mVideoContainer = findViewById(R.id.fl_video_container);
        mVideoAspectContainer = findViewById(R.id.video_aspect_container);
        mCicadaVodPlayerView = findViewById(R.id.video_view);
        mBtnGroup = findViewById(R.id.ll_btn_group);
        mFragmentContainer = findViewById(R.id.cicada_player_fm);

        mPerfDebugTextView = findViewById(R.id.tv_perf_debug);
        mHwVideoCodecName = PerfMonitorUtils.findHardwareVideoDecoderName();
        if (mHwVideoCodecName == null) {
            mHwVideoCodecName = "unknown";
        }

        // 方案1：启用 TunnelRender —— MediaCodec 直接渲染到 SurfaceView，
        // 跳过 SurfaceTexture + GL 逐帧绘制，大幅降低硬解播放 CPU。
        // 软解或硬解创建失败时播放器自动回退 GL 渲染（SuperMediaPlayer 内置链路）；
        // 字幕由 demo 层通过 SubtitleEvent 渲染，不受视频渲染管线影响
        PlayerConfig tunnelRenderConfig = mCicadaVodPlayerView.getPlayerConfig();
        tunnelRenderConfig.mEnableVideoTunnelRender = true;
        // 进度回调 500ms→1000ms：降低播放中主线程 UI 刷新频率
        tunnelRenderConfig.mPositionTimerIntervalMs = 1000;
        mCicadaVodPlayerView.setPlayerConfig(tunnelRenderConfig);


        //设置title
        tvTitle.setText(R.string.title_player);
        //保持屏幕敞亮
        mCicadaVodPlayerView.setKeepScreenOn(true);

        mScreenStatusController = new ScreenStatusController(this);

        //初始化artp的配置
        if(!TextUtils.isEmpty(urlDataSource) && urlDataSource.startsWith("artp")){
            PlayerConfig playerConfig = mCicadaVodPlayerView.getPlayerConfig();
            playerConfig.mMaxDelayTime = 100;
            mCicadaVodPlayerView.setPlayerConfig(playerConfig);
        }

    }

    private void initListener() {
        mPlayButton.setOnClickListener(this);
        mStopButton.setOnClickListener(this);
        mPauseButton.setOnClickListener(this);
        mRetryButton.setOnClickListener(this);
        mPrepareButton.setOnClickListener(this);
        mSnapShotButton.setOnClickListener(this);

        // 视频尺寸变化：对齐 ExoPlayer PlayerView —— 把视频宽高比交给官方
        // AspectRatioFrameLayout（RESIZE_MODE_FIT），隧道渲染直通下 surface
        // 始终与视频同比例，4:3/16:9/2.35:1/竖拍视频都不拉伸
        mCicadaVodPlayerView.setOnVideoSizeChangedListener(new CicadaPlayer.OnVideoSizeChangedListener() {
            @Override
            public void onVideoSizeChanged(int width, int height) {
                if (width > 0 && height > 0) {
                    mVideoWidth = width;
                    mVideoHeight = height;
                    mVideoAspectContainer.setAspectRatio((float) width / height);
                }
            }
        });

        // 全屏状态回调：竖拍视频全屏保持竖屏，也要隐藏按钮/标题等外围 UI
        mCicadaVodPlayerView.setOnScreenModeChangeListener(new CicadaVodPlayerView.OnScreenModeChangeListener() {
            @Override
            public void onScreenModeChanged(boolean fullScreen) {
                mFullScreenMode = fullScreen;
                updatePlayerViewMode();
            }
        });

        //准备完成回调
        mCicadaVodPlayerView.setOnPreparedListener(new CicadaPlayer.OnPreparedListener() {
            @Override
            public void onPrepared() {
                Toast.makeText(CicadaPlayerActivity.this, getString(R.string.cicada_prepare_success), Toast.LENGTH_SHORT).show();
                if(toFragment != null && toFragment instanceof PlayerTrackFragment){
                    ((PlayerTrackFragment) toFragment).initAll();
                }
            }
        });

        //截图监听回调
        mCicadaVodPlayerView.setSnapShotListener(new CicadaPlayer.OnSnapShotListener() {
            @Override
            public void onSnapShot(final Bitmap bitmap, int with, int height) {
                inSnapShotting = false;
                if(bitmap != null){
                    ThreadUtils.runOnSubThread(new Runnable() {
                        @Override
                        public void run() {
                            String bitmapPath = FileUtils.saveBitmap(bitmap);
                            if(!TextUtils.isEmpty(bitmapPath)){
                                SnapShotActivity.startScreenshotActivity(CicadaPlayerActivity.this,bitmapPath);
                            }
                        }
                    });
                }else{
                    Toast.makeText(CicadaPlayerActivity.this.getApplicationContext(), getString(R.string.cicada_snap_shot_failure), Toast.LENGTH_SHORT).show();
                }
            }
        });
        //外挂字幕
        mCicadaVodPlayerView.setOnOutSubtitleDisplayListener(new CicadaPlayer.OnSubtitleDisplayListener() {
            @Override
            public void onSubtitleExtAdded(int trackIndex, String url) {
                String name = "";
                if(url.equals(mSubtitleInfos.getCn())){
                    name = "cn";
                }else if(url.equals(mSubtitleInfos.getEn())){
                    name = "en";
                }else if(url.equals(mSubtitleInfos.getAss())){
                    name = "ass";
                }

                mSubtitleMap.put(trackIndex,name);
            }

            @Override
            public void onSubtitleShow(int i, long l, String s) {

            }

            @Override
            public void onSubtitleHide(int i, long l) {

            }

            @Override
            public void onSubtitleHeader(int trackIndex, String header) {

            }
        });

        //track变换回调
        mCicadaVodPlayerView.setOnTrackChangedListener(new CicadaPlayer.OnTrackChangedListener() {
            @Override
            public void onChangedSuccess(TrackInfo trackInfo) {
                if(toFragment != null && toFragment instanceof PlayerTrackFragment){
                    PlayerTrackFragment playerTrackFragment = (PlayerTrackFragment) toFragment;
                    playerTrackFragment.trackChanged(trackInfo);
                }
            }

            @Override
            public void onChangedFail(TrackInfo trackInfo, ErrorInfo errorInfo) {

            }
        });

        //设置底部 tab 监听
        mPlayerRaioGroup.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(RadioGroup group, int checkedId) {
                switch (checkedId) {
                    case R.id.checkbox_player_options:
                        turnToFragment(PlayerOperationFragment.class,null);
                        break;
                    case R.id.checkbox_player_config:
                        turnToFragment(PlayerConfigFragment.class,null);
                        break;
                    case R.id.checkbox_player_track:
                        turnToFragment(PlayerTrackFragment.class,null);
                        break;
                    case R.id.checkbox_cache_config:
                        turnToFragment(PlayerCacheConfigFragment.class,null);
                        break;
                    default:
                        turnToFragment(PlayerOperationFragment.class,null);
                        break;
                }
            }
        });


        //设置初始化默认选中状态
        ((RadioButton) mPlayerRaioGroup.findViewById(R.id.checkbox_player_options)).setChecked(true);

        /*
            设置屏幕装填监听
         */
        mScreenStatusController.setScreenStatusListener(new ScreenStatusController.ScreenStatusListener() {
            @Override
            public void onScreenOn() {
            }

            @Override
            public void onScreenOff() {
            }
        });
        mScreenStatusController.startListen();
    }

    @Override
    public void onClick(View v) {
        switch (v.getId()) {
            case R.id.prepare:
                //准备
                // 重新 prepare：重置解码模式标记与视频尺寸（新源可能走硬解/不同比例）
                mUseSoftwareDecoder = false;
                mVideoWidth = 0;
                mVideoHeight = 0;
                if(URL_TYPE.equals(playType)){
                    mCicadaVodPlayerView.setDataSource(urlDataSource);
                } else if(MANIFEST_TYPE.equals(playType)){
                    // 对象播放：MediaManifest JSON（接口返回，反序列化后传入）
                    mCicadaVodPlayerView.setDataSourceManifest(urlDataSource);
                }
                setPlayerSubtitleExt();
                mCicadaVodPlayerView.prepare();
                mRetryCount = 1;
                mRetryButton.setEnabled(false);
                break;
            case R.id.play:
                //播放
                mCicadaVodPlayerView.start();
                break;
            case R.id.stop:
                //停止
                mCicadaVodPlayerView.stop();
                break;
            case R.id.pause:
                //暂停
                mCicadaVodPlayerView.pause();
                break;
            case R.id.snapshot:
                //截图
                if(inSnapShotting){
                    Toast.makeText(this.getApplicationContext(), getString(R.string.cicada_snap_shotting), Toast.LENGTH_SHORT).show();
                }else{
                    boolean checkPermission = PermissionUtils.checkPermission(this, PermissionUtils.PERMISSION_MANIFEST[4]);
                    if(checkPermission){
                        mCicadaVodPlayerView.getSnapShot();
                        inSnapShotting = true;
                    }else{
                        PermissionUtils.requestPermissions(this,new String[]{PermissionUtils.PERMISSION_MANIFEST[4]},PERMISSION_REQUEST_CODE);
                    }

                }
                break;
            case R.id.retry:
                //重试
                mCicadaVodPlayerView.reload();
                mRetryButton.setEnabled(false);
                break;
            default:
                break;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQUEST_CODE) {
            boolean isAllGranted = true;

            // 判断是否所有的权限都已经授予了
            for (int grant : grantResults) {
                if (grant != PackageManager.PERMISSION_GRANTED) {
                    isAllGranted = false;
                    break;
                }
            }

            if (isAllGranted) {
                // 如果所有的权限都授予了
                mCicadaVodPlayerView.getSnapShot();
                inSnapShotting = true;
            } else {
                // 弹出对话框告诉用户需要权限的原因, 并引导用户去应用权限管理中手动打开权限按钮
                showPermissionDialog();
            }
        }
    }

    /**
     * 切换 Fragment
     *
     * @param toFragmentClass   要切换到的Fragment
     * @param args              需要传递的参数
     */
    public void turnToFragment(Class<? extends BaseFragment> toFragmentClass, Bundle args) {
        FragmentManager fm = getSupportFragmentManager();

        // 切换到的Fragment标签
        String toTag = toFragmentClass.getSimpleName();
        toFragment = (BaseFragment) fm.findFragmentByTag(toTag);
        // 如果要切换到的Fragment不存在,则创建
        if (toFragment == null) {
            try {
                toFragment = toFragmentClass.newInstance();
                if(URL_TYPE.equals(playType)){
                    toFragment.setPlaySource(urlDataSource);
                }
                toFragment.setArguments(args);
            } catch (InstantiationException e) {
                e.printStackTrace();
            } catch (IllegalAccessException e) {
                e.printStackTrace();
            }
        }

        // 如果有参数传递,
        if (args != null && !args.isEmpty()) {
            toFragment.getArguments().putAll(args);
        }

        // Fragment事务
        FragmentTransaction ft = fm.beginTransaction();
        // 注：support-v4 27 的 Fragment 切换动画在 Android 14+ 会反射
        // Animation.mListener（hidden API 被禁）导致 NoSuchFieldException 崩溃，
        // 此处去掉 setCustomAnimations
        // ft.setCustomAnimations(android.R.anim.fade_in, android.R.anim.fade_out,
        //         android.R.anim.fade_in, android.R.anim.fade_out);

        /*
         * 如果要切换到的Fragment没有被Fragment事务添加,则隐藏被切换的Fragment,添加要切换的Fragment
         * 否则,则隐藏被切换的Fragment,显示要切换的Fragment41
         */
        if (toFragment != null && !toFragment.isAdded()) {
            ft.hide(mCurrentFragment).add(R.id.cicada_player_fm, toFragment, toTag);
        } else {
            ft.hide(mCurrentFragment).show(toFragment);
        }

        //添加到返回堆栈
        ft.addToBackStack(FRAGMENT_BACK_STACK_NAME);
        //不保留状态提交事务
        ft.commitAllowingStateLoss();
        //将要展示的Fragment赋值给currentFragment
        mCurrentFragment = toFragment;
    }

    @Override
    protected void onResume() {
        super.onResume();

        updatePlayerViewMode();
        if (mCicadaVodPlayerView != null) {
            mCicadaVodPlayerView.onResume();
        }
        startPerfMonitor();
    }


    @Override
    protected void onStop() {
        super.onStop();

        stopPerfMonitor();
        if (mCicadaVodPlayerView != null && !mEnablePlayBack) {
            mCicadaVodPlayerView.onStop();
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        Log.d("lfj1019", " orientation = " + getResources().getConfiguration().orientation);
        updatePlayerViewMode();
    }

    private void updatePlayerViewMode() {
        if (mCicadaVodPlayerView == null) {
            return;
        }

        if (mFullScreenMode) {
            // 全屏（横屏视频→横屏全屏；竖拍视频→竖屏全屏）：
            // 隐藏标题栏/按钮区/Fragment 区，视频容器占满屏幕
            rlTitleRoot.setVisibility(View.GONE);
            if (mBtnGroup != null) {
                mBtnGroup.setVisibility(View.GONE);
            }
            if (mFragmentContainer != null) {
                mFragmentContainer.setVisibility(View.GONE);
            }
            if (!isStrangePhone()) {
                this.getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
                mCicadaVodPlayerView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
            }

            LinearLayout.LayoutParams fullLp = (LinearLayout.LayoutParams) mVideoContainer.getLayoutParams();
            fullLp.width = ViewGroup.LayoutParams.MATCH_PARENT;
            fullLp.height = ViewGroup.LayoutParams.MATCH_PARENT;
            mVideoContainer.setLayoutParams(fullLp);
            // 视频比例由官方 AspectRatioFrameLayout(RESIZE_MODE_FIT) 自动按视频
            // 宽高比测量，全屏/小窗/任意比例都等比留黑边
            return;
        }

        // 非全屏（竖屏小窗模式）
        rlTitleRoot.setVisibility(View.VISIBLE);
        if (mBtnGroup != null) {
            mBtnGroup.setVisibility(View.VISIBLE);
        }
        if (mFragmentContainer != null) {
            mFragmentContainer.setVisibility(View.VISIBLE);
        }
        this.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        mCicadaVodPlayerView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);

        LinearLayout.LayoutParams smallLp = (LinearLayout.LayoutParams) mVideoContainer.getLayoutParams();
        smallLp.width = ViewGroup.LayoutParams.MATCH_PARENT;
        // 视频尺寸未知（还没起播）才用 16:9 默认高度；否则保持原高度。
        // 小窗容器只是"可用空间"，视频区域由 AspectRatioFrameLayout 在其中
        // 等比居中（退后台回来也保持与之前一致）
        if (mVideoWidth <= 0 || mVideoHeight <= 0) {
            smallLp.height = (int) (ScreenUtils.getWidth(this) * 9.0f / 16);
        }
        mVideoContainer.setLayoutParams(smallLp);
    }

    /**
     * 启动性能调试监控：每秒刷新 解码方式 / App CPU / 系统 CPU / GPU 占用
     */
    private void startPerfMonitor() {
        if (mPerfDebugTextView == null) {
            return;
        }
        mLastProcCpu = PerfMonitorUtils.readProcessCpu();
        mLastSysCpu = PerfMonitorUtils.readSystemCpu();
        mLastPerfTickMs = System.currentTimeMillis();
        mPerfRunnable = new Runnable() {
            @Override
            public void run() {
                updatePerfDebugInfo();
                mPerfHandler.postDelayed(this, 1000);
            }
        };
        mPerfHandler.postDelayed(mPerfRunnable, 1000);
    }

    private void stopPerfMonitor() {
        if (mPerfRunnable != null) {
            mPerfHandler.removeCallbacks(mPerfRunnable);
            mPerfRunnable = null;
        }
    }

    private void updatePerfDebugInfo() {
        if (mPerfDebugTextView == null) {
            return;
        }
        long now = System.currentTimeMillis();
        long deltaMs = now - mLastPerfTickMs;
        if (deltaMs <= 0) {
            mLastPerfTickMs = now;
            return;
        }

        long[] procCpu = PerfMonitorUtils.readProcessCpu();
        long[] sysCpu = PerfMonitorUtils.readSystemCpu();

        double appCpu = PerfMonitorUtils.calcProcessCpuPercent(mLastProcCpu, procCpu, deltaMs);
        double sysCpuPercent = PerfMonitorUtils.calcSystemCpuPercent(mLastSysCpu, sysCpu);
        if (procCpu != null) {
            mLastProcCpu = procCpu;
        }
        if (sysCpu != null) {
            mLastSysCpu = sysCpu;
        }
        mLastPerfTickMs = now;

        String gpu = PerfMonitorUtils.readGpuUsage();
        if (gpu == null) {
            gpu = "不可读(需root)";
        }
        String decodeMode = mUseSoftwareDecoder
                ? "软解(FFmpeg)"
                : "硬解(" + mHwVideoCodecName + ")";
        String text = String.format(Locale.US,
                "解码: %s\nApp CPU: %.1f%%\nSys CPU: %.1f%%\nGPU: %s",
                decodeMode, appCpu, sysCpuPercent, gpu);
        mPerfDebugTextView.setText(text);
    }

    @Override
    protected void onDestroy() {
        releasePlayer();
        super.onDestroy();
    }

    private void releasePlayer() {
        if (mCicadaVodPlayerView != null) {
            mCicadaVodPlayerView.onDestroy();
            mCicadaVodPlayerView = null;
        }
        mScreenStatusController.stopListen();

        toFragment = null;
        mCurrentFragment  = null;
        mSubtitleMap.clear();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (mCicadaVodPlayerView != null) {
            boolean handler = mCicadaVodPlayerView.onKeyDown(keyCode, event);
            //handler == false 不要处理
            if (handler) {
                if(keyCode == KeyEvent.KEYCODE_BACK){
                    releasePlayer();
                    finish();
                }
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        //解决某些手机上锁屏之后会出现标题栏的问题。
        VcPlayerLog.d("lfj1030", "onWindowFocusChanged = " + hasFocus);
        updatePlayerViewMode();
    }

    /**
     * 设置缩放模式
     */
    public void setScaleMode(CicadaPlayer.ScaleMode scaleMode) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setScaleMode(scaleMode);
        }
    }

    /**
     * 设置镜像模式
     */
    public void setMirrorMode(CicadaPlayer.MirrorMode mirrorMode) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setMirrorMode(mirrorMode);
        }
    }

    /**
     * 设置旋转模式
     */
    public void setRotationMode(CicadaPlayer.RotateMode rotate) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setRotationMode(rotate);
        }
    }

    /**
     * 设置倍速播放
     */
    public void setSpeedMode(float speed) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setSpeedMode(speed);
        }
    }

    /**
     * 设置音量
     */
    public void setVolume(float volume) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setVolume(volume);
        }
    }

    /**
     * 设置播放配置
     *
     * @param referrer                      referrer
     * @param maxDelayTime                  最大延迟
     * @param httpProxy                     http代理
     * @param probeSize                     最大probe大小
     * @param netWorkTimeOut                网络超时时间
     * @param hightBufferLevel              高水位时长
     * @param firstStartBufferLevel         起播缓冲区时长
     * @param maxBufferPackedDuration       最大缓冲区时长
     * @param clearFrameWhenStop            停止显示最后帧
     * @param retryCount                    重试次数
     */
    public void setPlayerConfig(String referrer, String maxDelayTime, String httpProxy, String probeSize,
                                String netWorkTimeOut,String hightBufferLevel, String firstStartBufferLevel,
                                String maxBufferPackedDuration,boolean clearFrameWhenStop,String retryCount) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setPlayerConfig(referrer,maxDelayTime,httpProxy,probeSize,netWorkTimeOut,
                    hightBufferLevel,firstStartBufferLevel,maxBufferPackedDuration,clearFrameWhenStop,retryCount);
        }
    }

    /**
     * 设置CacheConfig
     * @param maxDuration   最大时长
     * @param maxSize       最大size
     * @param cacheDir      保存路径
     * @param enableCache   是否开启缓存
     */
    public void setCacheConfig(String maxDuration,String maxSize,String cacheDir,boolean enableCache){
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setCacheConfig(maxDuration,maxSize,cacheDir,enableCache);
        }
    }

    /**
     * 获取配置
     */
    public PlayerConfig getPlayConfig(){
        if(mCicadaVodPlayerView != null){
            return mCicadaVodPlayerView.getPlayerConfig();
        }
        return null;
    }


    /**
     * 设置是否自动播放
     */
    public void setAutoPlay(boolean isAutoPlay) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setAutoPlay(isAutoPlay);
        }
    }

    /**
     * 设置seek模式
     * @param isChecked true为精准seek,false为非精准seek
     */
    public void setSeekMode(boolean isChecked){
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setSeekMode(isChecked);
        }
    }

    /**
     * 设置是否循环
     */
    public void setLoop(boolean isLoop) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setLoop(isLoop);
        }
    }

    /**
     * 设置是否静音
     */
    public void setMute(boolean isMute) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.setMute(isMute);
        }
    }

    /**
     * 是否允许后台播放
     */
    public void enablePlayBack(boolean isChecked)
    {
        this.mEnablePlayBack = isChecked;
    }

    /**
     * 展示媒体信息
     */
    public void showMediaInfo() {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.showMediaInfo();
        }
    }

    /**
     * 选择外挂字幕TrackIndex
     */
    public void selectSubtitleTrackIndex(int trackIndex){
        if(mCicadaVodPlayerView != null){
            mLastTrackIndex = trackIndex;
            mCicadaVodPlayerView.selectSubtitleTrackIndex(trackIndex);
        }
    }

    /**
     * 获取TrackInfo信息
     */
    public TrackInfo getCurrentTrackInfo(TrackInfo.Type type) {
        if(mCicadaVodPlayerView != null){
            return mCicadaVodPlayerView.getCurrentTrackInfo(type);
        }
        return null;
    }

    /**
     * 获取MeidaInfo
     */
    public MediaInfo getMediaInfo() {
        if(mCicadaVodPlayerView != null){
            return mCicadaVodPlayerView.getMediaInfo();
        }
        return null;
    }

    /**
     * 设置 trackInfo
     */
    public void selectTrack(TrackInfo trackInfo) {
        if(mCicadaVodPlayerView != null){
            mCicadaVodPlayerView.selectTrack(trackInfo);
        }
    }

    public void setOnInfoListener(CicadaPlayer.OnInfoListener infoListener){
        if(mCicadaVodPlayerView != null){
            // 包装一层：外部监听（如 PlayerOperationFragment）照常收到事件，
            // 同时更新性能悬浮层的软解/硬解标记
            mCicadaVodPlayerView.setOnInfoListener(new CicadaPlayer.OnInfoListener() {
                @Override
                public void onInfo(InfoBean infoBean) {
                    if (infoBean != null && infoBean.getCode() == InfoCode.SwitchToSoftwareVideoDecoder) {
                        mUseSoftwareDecoder = true;
                    }
                    if (infoListener != null) {
                        infoListener.onInfo(infoBean);
                    }
                }
            });
        }
    }


    /**
     * 获取当前音量
     */
    public float getcurrentVolume() {
        if(mCicadaVodPlayerView != null){
            return mCicadaVodPlayerView.getVolume();
        }
        return 0;
    }

    public void enableHardwareDecoder(boolean enable) {
        if(mCicadaVodPlayerView != null){
             mCicadaVodPlayerView.enableHardwareDecoder(enable);
        }
    }

    /**
     * 获取已经添加的外挂字幕map
     */
    public LinkedHashMap<Integer,String> getSubtitleMap(){
        return mSubtitleMap;
    }

    /**
     * 设置外挂字幕
     */
    private void setPlayerSubtitleExt(){
        //设置url方式添加外挂字幕
        if(mSubtitleInfos != null){
            String cn = mSubtitleInfos.getCn();
            String en = mSubtitleInfos.getEn();
            if(!TextUtils.isEmpty(cn)){
                mCicadaVodPlayerView.addSubtitleExt(cn);
            }
            if(!TextUtils.isEmpty(en)){
                mCicadaVodPlayerView.addSubtitleExt(en);
            }
        }else{
            mSubtitleInfos = new PlayerMediaInfo.TypeInfo.SubtitleInfo();
            String assFilePath = Environment.getExternalStorageDirectory() + File.separator + "test.ass";
            mSubtitleInfos.setAss(assFilePath);
            mCicadaVodPlayerView.addSubtitleExt(assFilePath);
        }
    }
}

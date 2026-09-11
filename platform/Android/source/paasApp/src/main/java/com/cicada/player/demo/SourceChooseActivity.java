package com.cicada.player.demo;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.util.Log;
import android.support.annotation.NonNull;
import android.support.v7.widget.LinearLayoutManager;
import android.support.v7.widget.RecyclerView;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import com.cicada.player.demo.adapter.MediaInfoListTitleAdapter;
import com.cicada.player.demo.bean.PlayerMediaInfo;
import com.cicada.player.demo.listener.OnItemClickListener;
import com.cicada.player.demo.util.Common;
import com.cicada.player.demo.util.PermissionUtils;
import com.cicada.player.demo.util.SharedPreferenceUtils;
import com.cicada.player.demo.util.SourceListParser;
import com.cicada.player.demo.util.ThreadUtils;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.util.List;

/**
 * 选择资源Activity
 */
public class SourceChooseActivity extends BaseActivity implements View.OnClickListener, OnItemClickListener {

    private static final String TAG = "LocalVideoPicker";

    /**
     * FD 直连方案：播放期间保持打开的 ParcelFileDescriptor。
     * Android 11+ 分区存储下原始路径被拒绝访问，openFileDescriptor 拿到的 fd
     * 转成 /proc/self/fd/N 路径交给 FFmpeg 直接读，零复制秒开且可 seek
     */
    private static ParcelFileDescriptor sOpenVideoFd = null;

    /**
     * 权限请求码
     */
    private static final int PERMISSION_REQUEST_CODE = 1001;
    /**
     * 选择本地视频文件请求码
     */
    private static final int REQUEST_CODE_PICK_VIDEO = 2001;
    /**
     * 输入URL
     */
    private Button mInputUrlButton;

    /**
     * 选择本地视频
     */
    private Button mLocalVideoButton;

    /**
     * 该集合用于保存获取到的所有数据的所有播放方式,
     */
    private List<String> typeNameList;
    private RecyclerView recyclerView;
    private MediaInfoListTitleAdapter titleAdapter;

    /**
     * 权限
     */
    String[] permission = {
            Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.WRITE_EXTERNAL_STORAGE
    };

    private FrameLayout mDownloadFragmentLayout;
    private LinearLayout mRootLinearLayout;
    private TextView mRightTextView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        copyAssets();
        setContentView(R.layout.activity_source_choose);

        initView();
        initRecyclerView();
        initData();
        initListener();
        initFragment();
    }

    @Override
    protected void onStart() {
        super.onStart();
    }

    /**
     * 拷贝文件
     */
    private void copyAssets() {
        Common commenUtils = Common.getInstance(getApplicationContext()).copyAssetsToSD("subtitle", "aliyunPlayer");
        CustomFileOperateCallback mCustomFileOperateCallback = new CustomFileOperateCallback(this);
        commenUtils.setFileOperateCallback(mCustomFileOperateCallback);
    }


    private void initView() {
        TextView mTitleTextView = findViewById(R.id.tv_title);


        mInputUrlButton = findViewById(R.id.btn_input_url);

        mLocalVideoButton = findViewById(R.id.btn_local_video);

        mRootLinearLayout = findViewById(R.id.ll_root);


        recyclerView = findViewById(R.id.recycview_date_list);
        mDownloadFragmentLayout = findViewById(R.id.fl_download);

        mTitleTextView.setText(R.string.title_choose_item);

        mRightTextView = findViewById(R.id.tv_right);
        mRightTextView.setText(R.string.title_setting);
    }

    private void initListener() {

        mRightTextView.setOnClickListener(this);
        mInputUrlButton.setOnClickListener(this);
        mLocalVideoButton.setOnClickListener(this);
    }

    private void initFragment() {
    }


    private void initRecyclerView() {
        recyclerView.setNestedScrollingEnabled(true);
        LinearLayoutManager linearLayoutManager = new LinearLayoutManager(this, LinearLayoutManager.VERTICAL, false);
        recyclerView.setLayoutManager(linearLayoutManager);
    }

    private void initData() {
        boolean checkResult = PermissionUtils.checkPermissionsGroup(this, permission);
        if (!checkResult) {
            PermissionUtils.requestPermissions(this, permission, PERMISSION_REQUEST_CODE);
        } else {
            readListData();
        }
    }

    /**
     * 从 txt 中读取数据,并设置到RecyclerView中
     */
    private void readListData() {
        List<PlayerMediaInfo> mediaInfos = SourceListParser.parse(this);
        if (mediaInfos != null) {
            typeNameList = SourceListParser.getDateTitleKey(mediaInfos);
        }
        titleAdapter = new MediaInfoListTitleAdapter(typeNameList);
        recyclerView.setAdapter(titleAdapter);

        titleAdapter.setOnItemClickListener(this);
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
                //从sourceList.txt中读取数据
                readListData();
            } else {
                // 弹出对话框告诉用户需要权限的原因, 并引导用户去应用权限管理中手动打开权限按钮
                showPermissionDialog();
            }
        }
    }

    @Override
    public void onClick(View v) {
        switch (v.getId()) {
            case R.id.btn_input_url:
                //输入URL
                SourceInputUrlActivity.startSourceInputUrlActivity(this);

                break;
            case R.id.btn_local_video:
                //选择本地视频文件播放
                openSystemVideoPicker();
                break;
            case R.id.tv_right:
                //设置
                startActivity(SettingActivity.class);
                break;
            default:
                break;
        }
    }
    /**
     * RecyclerView item 点击事件
     */
    @Override
    public void onItemClick(int position) {
        Intent intent = new Intent(this, SourceChooseListActivity.class);
        intent.putExtra("typeName", typeNameList.get(position));
        startActivity(intent);
    }


    /**
     * 打开系统文件选择器选择本地视频（SAF，无需存储权限，兼容 Android 10+ 分区存储）
     */
    private void openSystemVideoPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("video/*");
        try {
            startActivityForResult(intent, REQUEST_CODE_PICK_VIDEO);
        } catch (Exception e) {
            Toast.makeText(this, R.string.cicada_pick_video_failed, Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_CODE_PICK_VIDEO && resultCode == RESULT_OK
                && data != null && data.getData() != null) {
            final Uri uri = data.getData();
            Log.i(TAG, "picked uri: " + uri);
            Toast.makeText(this, R.string.cicada_import_video_ing, Toast.LENGTH_SHORT).show();
            ThreadUtils.runOnSubThread(new Runnable() {
                @Override
                public void run() {
                    final String localPath = copyVideoToCache(uri);
                    ThreadUtils.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            if (TextUtils.isEmpty(localPath)) {
                                Toast.makeText(SourceChooseActivity.this,
                                        R.string.cicada_import_video_failed, Toast.LENGTH_SHORT).show();
                            } else {
                                // FFmpeg 播放器直接打开本地文件路径播放
                                CicadaPlayerActivity.startApsaraPlayerActivityByUrl(
                                        SourceChooseActivity.this, localPath);
                            }
                        }
                    });
                }
            });
        }
    }

    /**
     * 把 SAF 返回的 content:// Uri 变成可播放的本地路径。
     * 优先直接解析真实文件路径（本地存储的 provider 常能返回 _data），命中即零拷贝
     * 秒开；解析不到（如云盘、跨应用私有存储）才回退为复制到缓存目录。
     */
    private String copyVideoToCache(Uri uri) {
        String realPath = queryRealPath(uri);
        if (!TextUtils.isEmpty(realPath)) {
            File realFile = new File(realPath);
            if (realFile.exists() && realFile.canRead()) {
                closeVideoFd();
                Log.i(TAG, "use real path directly: " + realPath + " size=" + realFile.length());
                return realPath;
            }
        }

        // FD 直连仅用于 CicadaPlayer：FFmpeg 的 file 协议支持 /proc/self/fd 且
        // 容忍管道型 fd；ExoPlayer/MediaPlayer 的 FileDataSource 对管道型 fd
        // 不支持 seek 会报错，改为复制到缓存（兼容稳定）
        if (isCicadaPlayerSelected()) {
            String fdPath = openFdPath(uri);
            if (!TextUtils.isEmpty(fdPath)) {
                return fdPath;
            }
        }

        InputStream inputStream = null;
        FileOutputStream outputStream = null;
        try {
            // 清理历史缓存文件，避免越积越多
            File[] oldFiles = getCacheDir().listFiles();
            if (oldFiles != null) {
                for (File f : oldFiles) {
                    if (f.getName().startsWith("local_video_")) {
                        //noinspection ResultOfMethodCallIgnored
                        f.delete();
                    }
                }
            }

            // 尽量沿用原始文件名后缀，便于播放器按扩展名识别封装格式
            String ext = "mp4";
            Cursor cursor = getContentResolver().query(uri,
                    new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (cursor != null) {
                try {
                    if (cursor.moveToFirst()) {
                        String name = cursor.getString(0);
                        if (name != null) {
                            int dot = name.lastIndexOf('.');
                            if (dot >= 0 && dot < name.length() - 1) {
                                String candidate = name.substring(dot + 1);
                                if (candidate.matches("[A-Za-z0-9]{1,10}")) {
                                    ext = candidate.toLowerCase();
                                }
                            }
                        }
                    }
                } finally {
                    cursor.close();
                }
            }

            File cacheFile = new File(getCacheDir(),
                    "local_video_" + System.currentTimeMillis() + "." + ext);
            inputStream = getContentResolver().openInputStream(uri);
            if (inputStream == null) {
                return null;
            }
            outputStream = new FileOutputStream(cacheFile);
            byte[] buffer = new byte[64 * 1024];
            int len;
            while ((len = inputStream.read(buffer)) > 0) {
                outputStream.write(buffer, 0, len);
            }
            outputStream.flush();
            long fileSize = cacheFile.length();
            Log.i(TAG, "copied to: " + cacheFile.getAbsolutePath() + " size=" + fileSize);
            if (fileSize <= 0) {
                Log.e(TAG, "copied file is empty, abort");
                return null;
            }
            return cacheFile.getAbsolutePath();
        } catch (Exception e) {
            return null;
        } finally {
            try {
                if (inputStream != null) {
                    inputStream.close();
                }
            } catch (IOException ignored) {
            }
            try {
                if (outputStream != null) {
                    outputStream.close();
                }
            } catch (IOException ignored) {
            }
        }
    }

    /**
     * 当前 demo 选择的播放器内核是否为 CicadaPlayer
     */
    private boolean isCicadaPlayerSelected() {
        String playerName = SharedPreferenceUtils.getStringExtra(SharedPreferenceUtils.SELECTED_PLAYER_NAME);
        if ("CicadaPlayer".equals(playerName)) {
            return true;
        }
        if (TextUtils.isEmpty(playerName)) {
            return SharedPreferenceUtils.getBooleanExtra(SharedPreferenceUtils.SELECTED_CICADA_PLAYER);
        }
        return false;
    }

    /**
     * FD 直连：openFileDescriptor 拿 fd，转成 /proc/self/fd/N 路径。
     * 本地文件的 fd 支持 seek（等价原文件）；云盘流的 fd 可顺序读。
     * fd 在播放期间保持打开（静态引用），选新文件时替换并关闭旧的。
     */
    private String openFdPath(Uri uri) {
        try {
            closeVideoFd();
            sOpenVideoFd = getContentResolver().openFileDescriptor(uri, "r");
            if (sOpenVideoFd == null) {
                return null;
            }
            String fdPath = "/proc/self/fd/" + sOpenVideoFd.getFd();
            if (!new File(fdPath).canRead()) {
                closeVideoFd();
                return null;
            }
            Log.i(TAG, "use fd path (zero copy): " + fdPath);
            return fdPath;
        } catch (Exception e) {
            Log.w(TAG, "openFileDescriptor failed: " + e);
            closeVideoFd();
            return null;
        }
    }

    private static void closeVideoFd() {
        if (sOpenVideoFd != null) {
            try {
                sOpenVideoFd.close();
            } catch (Exception ignored) {
            }
            sOpenVideoFd = null;
        }
    }

    /**
     * 尝试把 content:// Uri 解析为真实文件绝对路径，命中即零拷贝秒开：
     * 1) file:// scheme 直接取 path；
     * 2) DocumentsProvider 的 documentId（raw:/primary:/SD 卡卷），覆盖系统
     *    文件选择器的 Downloads/相册/文件管理来源；
     * 3) MediaStore._data（旧路径兼容）。
     * 全部失败返回 null（由调用方回退到复制方案）。
     */
    private String queryRealPath(Uri uri) {
        if (uri == null) {
            return null;
        }

        if ("file".equalsIgnoreCase(uri.getScheme())) {
            return uri.getPath();
        }

        // 1) DocumentsProvider documentId 解析
        String docId = null;
        try {
            docId = DocumentsContract.getDocumentId(uri);
        } catch (Exception ignored) {
        }
        if (!TextUtils.isEmpty(docId)) {
            // 形如 raw:/storage/emulated/0/Movies/video.mp4
            if (docId.startsWith("raw:")) {
                return docId.substring("raw:".length());
            }
            // 形如 primary:Movies/video.mp4 → 内置存储根 + 相对路径
            if (docId.startsWith("primary:")) {
                String path = Environment.getExternalStorageDirectory().getAbsolutePath()
                        + "/" + docId.substring("primary:".length());
                if (new File(path).canRead()) {
                    return path;
                }
            }
            // 其它卷（SD 卡）：形如 1234-5678:Movies/video.mp4 → /storage/XXXX-XXXX/相对路径
            String[] parts = docId.split(":", 2);
            if (parts.length == 2 && !TextUtils.isEmpty(parts[1])) {
                File storageRoot = new File("/storage");
                File[] volumes = storageRoot.listFiles();
                if (volumes != null) {
                    for (File root : volumes) {
                        File candidate = new File(root, parts[1]);
                        if (candidate.exists() && candidate.canRead()) {
                            return candidate.getAbsolutePath();
                        }
                    }
                }
            }
        }

        // 2) MediaStore._data（部分旧 provider / 媒体库 Uri）
        Cursor cursor = null;
        try {
            cursor = getContentResolver().query(uri,
                    new String[] {MediaStore.Video.Media.DATA}, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                int idx = cursor.getColumnIndex(MediaStore.Video.Media.DATA);
                if (idx >= 0) {
                    String path = cursor.getString(idx);
                    if (!TextUtils.isEmpty(path)) {
                        return path;
                    }
                }
            }
        } catch (Exception ignored) {
            // 部分 provider 不支持 _data 投影，继续回退
        } finally {
            if (cursor != null) {
                cursor.close();
            }
        }
        return null;
    }

    @Override
    public void onBackPressed() {
        if (mDownloadFragmentLayout.isShown()) {
            //隐藏下载界面
            mRootLinearLayout.setVisibility(View.VISIBLE);
            mDownloadFragmentLayout.setVisibility(View.GONE);
            return;
        }
        super.onBackPressed();
    }

    private static class CustomFileOperateCallback implements Common.FileOperateCallback {

        private WeakReference<SourceChooseActivity> weakReference;

        public CustomFileOperateCallback(SourceChooseActivity sourceChooseActivity) {
            weakReference = new WeakReference<>(sourceChooseActivity);
        }

        @Override
        public void onSuccess() {
        }

        @Override
        public void onFailed(String error) {
            ThreadUtils.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    SourceChooseActivity sourceChooseActivity = weakReference.get();
                    if (sourceChooseActivity != null) {
                        Toast.makeText(sourceChooseActivity, sourceChooseActivity.getString(R.string.cicada_copy_file_failure), Toast.LENGTH_SHORT).show();
                    }
                }
            });
        }
    }
}

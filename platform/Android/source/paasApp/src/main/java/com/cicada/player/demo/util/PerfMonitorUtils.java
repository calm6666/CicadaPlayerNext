package com.cicada.player.demo.util;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.util.Locale;

/**
 * 播放性能调试工具：
 * <ul>
 *   <li>进程 / 系统 CPU 占用：/proc/self/stat、/proc/stat（无需任何权限）</li>
 *   <li>GPU 占用：厂商 sysfs 节点（高通部分机型免 root 可读，Mali/三星通常需 root，
 *       读不到返回 null，由调用方显示"不可读(需root)"）</li>
 *   <li>硬件视频解码器名：MediaCodecList 查询（用于展示当前硬解使用的 codec）</li>
 * </ul>
 */
public class PerfMonitorUtils {

    private static final String TAG = "PerfMonitor";

    /**
     * Android USER_HZ = 100（sysconf(_SC_CLK_TCK)），/proc 里的 jiffies 按 100Hz 计
     */
    private static final double TICKS_PER_SECOND = 100.0;

    /**
     * 常见 GPU 占用节点，按序尝试，读到第一个可读的
     */
    private static final String[] GPU_BUSY_NODES = {
            // 高通 Adreno（部分机型 0444 免 root 可读）
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
            "/sys/devices/platform/kgsl-3d0/kgsl/kgsl-3d0/gpu_busy_percentage",
            // 高通 gpubusy："busy total" 两列
            "/sys/class/kgsl/kgsl-3d0/gpubusy",
            // 联发科/麒麟 Mali（通常 0440 需 root）
            "/sys/class/misc/mali0/device/utilization",
            // 三星 Exynos（通常需 root）
            "/sys/kernel/gpu/gpu_busy",
    };

    private static final String[] HW_VIDEO_MIMES = {
            "video/avc", "video/hevc", "video/x-vnd.on2.vp9", "video/av01", "video/x-vnd.on2.vp8"
    };

    /**
     * 应用进程 CPU 时间（utime+stime，jiffies），失败返回 null
     */
    public static long[] readProcessCpu() {
        BufferedReader reader = null;
        try {
            reader = new BufferedReader(new InputStreamReader(new FileInputStream("/proc/self/stat")));
            String line = reader.readLine();
            if (line == null) {
                return null;
            }
            // 进程名可能带空格，从最后一个 ')' 之后切分；
            // 切分后下标 11=utime、12=stime（原始字段号 14、15）
            int idx = line.lastIndexOf(')');
            String[] fields = line.substring(idx + 2).trim().split("\\s+");
            if (fields.length < 13) {
                return null;
            }
            long utime = Long.parseLong(fields[11]);
            long stime = Long.parseLong(fields[12]);
            return new long[] {utime + stime};
        } catch (Exception e) {
            return null;
        } finally {
            closeQuietly(reader);
        }
    }

    /**
     * 系统 CPU（/proc/stat 首行），返回 {total, idle}，失败返回 null
     */
    public static long[] readSystemCpu() {
        BufferedReader reader = null;
        try {
            reader = new BufferedReader(new InputStreamReader(new FileInputStream("/proc/stat")));
            String line = reader.readLine();
            if (line == null || !line.startsWith("cpu ")) {
                return null;
            }
            // cpu user nice system idle iowait irq softirq steal guest guest_nice
            String[] f = line.trim().split("\\s+");
            long total = 0;
            for (int i = 1; i < f.length; i++) {
                total += Long.parseLong(f[i]);
            }
            long idle = Long.parseLong(f[4]) + Long.parseLong(f[5]); // idle + iowait
            return new long[] {total, idle};
        } catch (Exception e) {
            return null;
        } finally {
            closeQuietly(reader);
        }
    }

    /**
     * 进程 CPU 占用百分比（单核口径，与 top 一致；多核时可能超过 100%）
     */
    public static double calcProcessCpuPercent(long[] prev, long[] curr, long deltaWallMs) {
        if (prev == null || curr == null || deltaWallMs <= 0) {
            return 0;
        }
        long deltaTicks = curr[0] - prev[0];
        return deltaTicks * 1000.0 / deltaWallMs / TICKS_PER_SECOND * 100.0;
    }

    /**
     * 系统整体 CPU 忙百分比（0~100）
     */
    public static double calcSystemCpuPercent(long[] prev, long[] curr) {
        if (prev == null || curr == null) {
            return 0;
        }
        long dTotal = curr[0] - prev[0];
        long dIdle = curr[1] - prev[1];
        if (dTotal <= 0) {
            return 0;
        }
        return Math.max(0, 100.0 - dIdle * 100.0 / dTotal);
    }

    /**
     * GPU 占用百分比字符串（如 "58.0%"）；读不到返回 null
     */
    public static String readGpuUsage() {
        for (String node : GPU_BUSY_NODES) {
            BufferedReader reader = null;
            try {
                File f = new File(node);
                if (!f.exists() || !f.canRead()) {
                    continue;
                }
                reader = new BufferedReader(new InputStreamReader(new FileInputStream(f)));
                String line = reader.readLine();
                if (line == null) {
                    continue;
                }
                line = line.trim();
                if (line.isEmpty()) {
                    continue;
                }
                String[] parts = line.split("\\s+");
                if (parts.length >= 2) {
                    // 形如 "12345 67890"（busy total 两列，高通 gpubusy 风格）
                    double busy = Double.parseDouble(parts[0]);
                    double total = Double.parseDouble(parts[1]);
                    if (total > 0 && busy >= 0 && busy <= total) {
                        return String.format(Locale.US, "%.1f%%", busy * 100.0 / total);
                    }
                }
                double v = Double.parseDouble(parts[0]);
                if (v >= 0 && v <= 100) {
                    return String.format(Locale.US, "%.1f%%", v);
                }
            } catch (Exception ignored) {
                // 该节点不可读/格式不识别，继续尝试下一个
            } finally {
                closeQuietly(reader);
            }
        }
        return null;
    }

    /**
     * 查找本机硬件视频解码器名（优先 avc，其次 hevc/vp9/av1/vp8），
     * 找不到返回 null
     */
    public static String findHardwareVideoDecoderName() {
        try {
            MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
            for (String mime : HW_VIDEO_MIMES) {
                for (MediaCodecInfo info : list.getCodecInfos()) {
                    if (info.isEncoder()) {
                        continue;
                    }
                    String name = info.getName();
                    String lower = name.toLowerCase();
                    // 过滤软件解码器（c2.android.* / OMX.google.* / 带 software 字样）
                    if (lower.contains("software")
                            || name.startsWith("c2.android.")
                            || name.startsWith("OMX.google.")) {
                        continue;
                    }
                    for (String supported : info.getSupportedTypes()) {
                        if (mime.equalsIgnoreCase(supported)) {
                            return name;
                        }
                    }
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "findHardwareVideoDecoderName failed", e);
        }
        return null;
    }

    private static void closeQuietly(BufferedReader reader) {
        if (reader == null) {
            return;
        }
        try {
            reader.close();
        } catch (Exception ignored) {
        }
    }
}

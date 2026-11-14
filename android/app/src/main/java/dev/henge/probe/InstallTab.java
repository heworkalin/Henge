package dev.henge.probe;

import android.app.Activity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The Install tab: choose a macOS, download it, install it.
 *
 * The iOS InstallView, in the same order and for the same reason: the version comes first because
 * it is the only decision, the transfer second because it is the long part, and the log last
 * because when something goes wrong the log is what says where.
 *
 * Nothing here is a shortcut for the user. The image is downloaded from Apple rather than shipped,
 * sixteen gigabytes at a time; it resumes across app restarts because a `.part` file and a range
 * request make that free; and the install that follows is the guest's own restore, running over the
 * machine's USB port, exactly as it does on a Mac.
 */
final class InstallTab {

    private static final String ROM_NAME = BootRom.NAME;
    private static final String DISK_NAME = "disk.img";
    private static final String AUX_NAME = "aux.img";
    /** 64 GiB, sparse: address space now, storage only as macOS writes to it. */
    private static final long DISK_BYTES = 64L << 30;

    /** Picker request code; MainActivity hands the result back to {@link #imported}. */
    static final int REQUEST_IPSW = 501;

    private InstallTab() {
    }

    /**
     * Takes the URI the picker returned and makes it usable, then stores it.
     *
     * Called from the activity's result dispatch, so it runs on the UI thread; the copy, when one
     * is needed, happens on its own thread.
     */
    static void imported(Activity a, android.net.Uri uri) {
        File dir = dir(a);
        new Thread(() -> {
            IpswImport.Result result = IpswImport.take(a, uri, dir, text ->
                    AppLog.i("导入: " + text));
            a.runOnUiThread(() -> {
                if (result.error != null) {
                    AppLog.w("导入: " + result.error, null);
                } else {
                    Prefs.setLocalIpsw(a, result.path);
                    AppLog.i("导入: 使用 " + result.path);
                }
                // Rebuilding is the cheapest way to show the new value; the tab is one screen.
                if (a instanceof MainActivity) ((MainActivity) a).reloadTab();
            });
        }, "henge-import").start();
    }

    static View build(Activity a) {
        LinearLayout root = Ui.column(a);
        root.addView(Ui.title(a, "安装 macOS"));

        ScrollView scroll = new ScrollView(a);
        scroll.setBackgroundColor(Theme.surface(a));
        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(Theme.surface(a));
        scroll.addView(content, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        TextView status = Ui.body(a, "");
        content.addView(status);

        // ---- the only decision: which macOS ----
        LinearLayout releases = Lists.group(a, content, "版本");
        File dir = dir(a);
        Releases.Release[] chosen = {Releases.byBuild(Prefs.releaseBuild(a))};
        TextView[] marks = new TextView[Releases.ALL.length];

        // What each row says on its right: the download's state, or that it is the one chosen.
        // Painted from one place so a change of mind cannot leave two rows marked.
        Runnable paint = () -> {
            for (int i = 0; i < Releases.ALL.length; i++) {
                Releases.Release r = Releases.ALL[i];
                if (marks[i] == null) continue;
                if (r == chosen[0]) {
                    marks[i].setText(r == Releases.RECOMMENDED ? "推荐 · 已选择" : "已选择");
                } else if (Releases.ipswFile(dir, r).exists()) {
                    marks[i].setText("已下载");
                } else if (Releases.partFile(dir, r).exists()) {
                    marks[i].setText("已下 " + AppLog.bytes(Releases.partFile(dir, r).length()));
                } else {
                    marks[i].setText(AppLog.bytes(r.bytes));
                }
            }
        };

        for (int i = 0; i < Releases.ALL.length; i++) {
            final int index = i;
            Releases.Release r = Releases.ALL[i];
            String state = r.tested ? "已测试" : "实验性 · 未测试";
            Lists.LiveRow row = Lists.liveRow(a, releases, r.name, r.build + " · " + state + " · " + r.note,
                    "", false, null);
            marks[i] = row.value;
            row.view.setOnClickListener(v -> {
                chosen[0] = r;
                Prefs.setReleaseBuild(a, r.build);
                AppLog.i("安装: 选择 " + r.name + (r.tested ? "" : "（实验性，未测试）"));
                if (!r.tested)
                    Toast.makeText(a, "实验性版本：本机没有测试过，可能装不上", Toast.LENGTH_SHORT).show();
                paint.run();
            });
            if (i < Releases.ALL.length - 1) Lists.divider(a, releases);
        }
        paint.run();

        // ---- an image the user already has ----
        LinearLayout imported = Lists.group(a, content, "本地 IPSW");
        Lists.LiveRow localRow = Lists.liveRow(a, imported, "导入 IPSW…",
                "选一个已下载好的 .ipsw，直接安装", localLabel(a), true, () -> {
                    android.content.Intent intent = new android.content.Intent(
                            android.content.Intent.ACTION_OPEN_DOCUMENT);
                    intent.addCategory(android.content.Intent.CATEGORY_OPENABLE);
                    intent.setType("*/*");
                    intent.addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    a.startActivityForResult(intent, REQUEST_IPSW);
                });

        // ---- the transfer ----
        LinearLayout transfer = Lists.group(a, content, "下载");
        final TextView progress = new TextView(a);
        progress.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 17f);
        progress.setTextColor(Theme.onSurface(a));
        progress.setLineSpacing(Ui.dp(a, 4), 1f);
        progress.setPadding(Ui.dp(a, 16), Ui.dp(a, 16), Ui.dp(a, 16), Ui.dp(a, 8));
        transfer.addView(progress);

        // A bar under the words: a percentage alone makes the user divide numbers to know whether
        // it is moving, and the bar answers "is it going" at a glance from across the room.
        final android.widget.ProgressBar bar = new android.widget.ProgressBar(a, null,
                android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(Theme.primary(a)));
        bar.setProgressBackgroundTintList(
                android.content.res.ColorStateList.valueOf(Theme.withAlpha(Theme.primary(a), 0.18f)));
        LinearLayout.LayoutParams barLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(a, 6));
        barLp.setMargins(Ui.dp(a, 16), 0, Ui.dp(a, 16), Ui.dp(a, 8));
        transfer.addView(bar, barLp);

        // One button, and it says what pressing it does right now: download, or stop. A second
        // "stop" button next to it was a permanent grey rectangle for a state that is rare.
        LinearLayout buttons = new LinearLayout(a);
        buttons.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(a, 12);
        buttons.setPadding(pad, Ui.dp(a, 8), pad, Ui.dp(a, 12));
        final AtomicBoolean cancelled = new AtomicBoolean(false);
        final boolean[] busy = {false};
        final Downloader.Progress[] sink = new Downloader.Progress[1];
        final android.widget.Button action = Ui.primaryButton(a, "下载并安装", () -> { });
        buttons.addView(action, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        transfer.addView(buttons);

        Runnable refreshWaiting = () -> {
            Releases.Release r = chosen[0];
            File ipsw = Releases.ipswFile(dir, r);
            File part = Releases.partFile(dir, r);
            if (ipsw.exists() && ipsw.length() > 0) {
                showProgress(progress, bar, "已下载 " + r.name + "\n"
                        + AppLog.bytes(ipsw.length()) + "，可以直接安装");
                bar.setProgress(1000);
            } else if (part.exists()) {
                int percent = (int) (part.length() * 100 / Math.max(1, r.bytes));
                showProgress(progress, bar, "已下载 " + percent + "%（"
                        + AppLog.bytes(part.length()) + " / " + AppLog.bytes(r.bytes)
                        + "）\n再次开始会从断点继续");
            } else {
                showProgress(progress, bar, "尚未下载\n" + r.name + " · " + AppLog.bytes(r.bytes));
                bar.setProgress(0);
            }
        };
        refreshWaiting.run();

        final boolean[] installing = {false};

        /**
         * One button, four meanings, decided by the state on screen: download, stop the download,
         * install what is already here, or stop the install.
         */
        action.setOnClickListener(v -> {
            Releases.Release r = chosen[0];
            File ipsw = Releases.ipswFile(dir, r);

            if (DownloadService.running()) {
                DownloadService.cancel(a);
                action.setText("正在停止…");
                return;
            }
            if (installing[0]) {
                cancelled.set(true);
                action.setText("正在停止…");
                InstallService.requestStop(a);
                return;
            }
            if (InstallService.running()) {
                /* A run this screen is not following (the previous attempt's service outlived its
                 * thread). End it and let the next tap start a fresh one; marking this screen as
                 * installing would make the next tap stop a run it never started. */
                action.setText("正在停止上一次…");
                InstallService.requestStop(a);
                return;
            }

            String local = Prefs.localIpsw(a);
            File localFile = local == null ? null : new File(local);
            boolean haveImage = (localFile != null && localFile.exists()
                    && localFile.length() > (1L << 30))
                    || (ipsw.exists() && ipsw.length() > 0);

            if (!haveImage) {
                // The download belongs to the service, not to this screen: that is what keeps it
                // alive when the user goes and does something else for an hour.
                AppLog.i("下载: 请求服务下载 " + r.name);
                Permissions.askForDownload(a);
                DownloadService.start(a, r);
                action.setText("停止");
                return;
            }

            File source = (localFile != null && localFile.exists()) ? localFile : ipsw;
            installing[0] = true;
            cancelled.set(false);
            action.setText("停止安装");
            final boolean ours = (localFile == null);   // downloaded by this app, so it may be re-fetched
            // A restore is the one operation where being reclaimed costs an hour.
            final int run = InstallService.start(a);
            new Thread(() -> {
                /*
                 * Check the image before touching the machine. Nothing downstream does: libzip
                 * reads without verifying and idevicerestore trusts what it reads, so a file that
                 * went bad would be personalised and sent to the guest as if it were good, and the
                 * failure would show up as firmware that misbehaves for no visible reason.
                 */
                /*
                 * Once per file, not once per install: a verified image is remembered by path,
                 * length and modification time, so a second attempt at the same broken install
                 * starts immediately instead of reading 16 GB again.
                 */
                IpswIntegrity.Result check = null;
                if (Prefs.isVerified(a, source)) {
                    AppLog.i("安装: 镜像已校验过，跳过校验（" + source.getName() + "）");
                } else {
                    a.runOnUiThread(() -> showProgress(progress, bar, "校验镜像…"));
                    check = IpswIntegrity.verify(source,
                            line -> a.runOnUiThread(() -> showProgress(progress, bar, line)),
                            cancelled);
                }
                if (check != null && !check.ok) {
                    String why = check.problem;
                    AppLog.w("安装: 镜像校验失败: " + why, null);
                    Prefs.clearVerified(a);
                    if (ours && !"已取消".equals(why)) {
                        // Only a file this app downloaded is ours to delete; a path the user picked
                        // stays theirs, however broken it is.
                        a.runOnUiThread(() -> showProgress(progress, bar,
                                "镜像校验失败，已删除，将重新下载\n" + why));
                        source.delete();
                        cancelled.set(false);
                        a.runOnUiThread(() -> DownloadService.start(a, chosen[0]));
                        installing[0] = false;
                        a.runOnUiThread(() -> action.setText("下载并安装"));
                        return;
                    }
                    installing[0] = false;
                    InstallService.stop(a, run);
                    a.runOnUiThread(() -> {
                        action.setText("下载并安装");
                        showProgress(progress, bar, "镜像校验失败，请检查这个文件或重新下载\n" + why);
                    });
                    return;
                }
                if (check != null) Prefs.markVerified(a, source);
                String outcome = install(a, dir, source, progress, cancelled);
                installing[0] = false;
                InstallService.stop(a, run);
                a.runOnUiThread(() -> {
                    action.setText("下载并安装");
                    if (outcome == null) {
                        showProgress(progress, bar, "安装完成。回到 Macs 页按「启动」。");
                    } else if ("已取消".equals(outcome)) {
                        showProgress(progress, bar, "已停止。");
                        cancelled.set(false);
                    } else {
                        showProgress(progress, bar, "失败: " + outcome);
                    }
                });
            }, "henge-install").start();
        });

        // The screen follows the service rather than the other way round: the download outlives
        // this view, so the view reads it instead of owning it.
        Runnable follow = () -> a.runOnUiThread(() -> {
            if (installing[0]) return;
            DownloadService.State state = DownloadService.state();
            String phase = state.phase;
            if (DownloadService.State.RUNNING.equals(phase)) {
                action.setText("停止");
                int percent = state.total > 0 ? (int) (state.received * 100 / state.total) : 0;
                bar.setProgress(percent * 10);
                StringBuilder text = new StringBuilder("正在下载 ").append(state.name)
                        .append('\n')
                        .append(AppLog.bytes(state.received)).append(" / ")
                        .append(AppLog.bytes(state.total)).append("（").append(percent).append("%）");
                if (state.speed > 0) {
                    text.append('\n').append(AppLog.bytes(state.speed)).append("/s");
                    if (state.etaMs > 0) text.append(" · 还需 ").append(AppLog.eta(state.etaMs));
                } else {
                    text.append("\n正在探测网速…");
                }
                progress.setText(text.toString());
            } else if (DownloadService.State.DONE.equals(phase)) {
                action.setText("安装");
                bar.setProgress(1000);
                progress.setText("已下载 " + state.name + "\n点「安装」开始写入磁盘");
            } else if (DownloadService.State.FAILED.equals(phase)) {
                action.setText("重试");
                progress.setText("下载失败: " + state.message);
            } else if (DownloadService.State.CANCELLED.equals(phase)) {
                action.setText("继续下载");
                progress.setText("已停止，已下载的部分会保留\n" + state.message);
            } else {
                action.setText("下载并安装");
                refreshWaiting.run();
            }
        });
        DownloadService.listen(follow);
        follow.run();


        return root;
    }

    /** The percentage inside a status line, or 0 when it carries none. */
    private static int percentOf(String text) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(\\d{1,3})%").matcher(text);
        return m.find() ? Integer.parseInt(m.group(1)) : 0;
    }

    /** Sets the state line and, when it carries a percentage, the bar under it. */
    private static void showProgress(TextView text, android.widget.ProgressBar bar, String message) {
        text.setText(message);
        java.util.regex.Matcher m = java.util.regex.Pattern
                .compile("(\\d{1,3})%").matcher(message);
        if (m.find()) {
            bar.setProgress(Math.min(1000, Integer.parseInt(m.group(1)) * 10));
        } else if (message.contains("完成")) {
            bar.setProgress(1000);
        }
    }

    private static String localLabel(Activity a) {
        String path = Prefs.localIpsw(a);
        if (path == null) return "无";
        File f = new File(path);
        if (!f.exists()) return "已失效";
        return AppLog.bytes(f.length());
    }

    private static File dir(Activity a) {
        return new File(a.getExternalFilesDir(null), "vm");
    }

    /**
     * Takes the machine from an image to a running restore: images, ROM, DFU boot, then poll.
     *
     * Returns null when the restore finished, or a sentence saying why it did not.
     */
    private static String install(Activity a, File dir, File ipsw, TextView progress,
                                  AtomicBoolean cancelled) {
        File disk = new File(dir, DISK_NAME);
        File aux = new File(dir, AUX_NAME);
        File work = new File(dir, "work");

        a.runOnUiThread(() -> progress.setText("准备引导 ROM…"));
        // The ROM is not in the IPSW — it is inside Apple's system image, which is why this is a
        // separate, small download. See BootRom.
        String problem = BootRom.prepare(dir);
        if (problem != null) return problem;
        File rom = new File(dir, ROM_NAME);
        if (!work.exists() && !work.mkdirs()) return "无法创建 " + work;

        String prepared = Native.nativeInstallPrepare(dir.getAbsolutePath(),
                disk.getAbsolutePath(), aux.getAbsolutePath(), DISK_BYTES);
        AppLog.i("安装: " + prepared.replace('\n', ' '));

        String started;
        try {
            started = Native.nativeInstallStart(rom.getAbsolutePath(), disk.getAbsolutePath(),
                    aux.getAbsolutePath(), ipsw.getAbsolutePath(), work.getAbsolutePath(),
                    Prefs.ramMb(a), Prefs.cpus(a), 0x1234567890abcdefL,
                    a.getCacheDir() == null ? null : a.getCacheDir().getAbsolutePath());
        } catch (Throwable t) {
            AppLog.w("安装: 启动异常", t);
            return "启动异常: " + t;
        }
        AppLog.i("安装: " + started.replace('\n', ' '));
        final String first = started;
        a.runOnUiThread(() -> progress.setText(first));

        while (true) {
            if (cancelled.get()) return "已取消";
            String state;
            try {
                state = Native.nativeInstallStatus();
            } catch (Throwable t) {
                return "读状态异常: " + t;
            }
            final String shown = state;
            a.runOnUiThread(() -> {
                progress.setText(shown);
                // The notification says the same thing as the screen: a user who left the app to
                // do something else for a while should not have to come back to find out.
                InstallService.update(a, shown, percentOf(shown));
            });
            if (state.contains("完成")) return null;
            if (state.contains("失败")) return state;
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return "已取消";
            }
        }
    }
}

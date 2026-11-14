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
        /* Filled in once the transfer section exists: choosing a version has to repaint what that
           section says, or the last version's "failed" stays on screen as if it were this one's. */
        final Runnable[] repaint = new Runnable[1];

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
                    long got = Downloader.held(Releases.partFile(dir, r),
                            new File(Releases.partFile(dir, r).getPath() + ".chunks"), r.bytes);
                    marks[i].setText("已下 " + AppLog.bytes(got));
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
                if (repaint[0] != null) repaint[0].run();
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
        /* What this screen showed before it was rebuilt. */
        if (lastText != null) progress.setText(lastText);
        if (lastBar >= 0) bar.setProgress(lastBar);
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
                /* From the chunk marks, not the file's length: the file is pre-sized to the whole
                   image, so its length would report a download that just started as complete. */
                long got = Downloader.held(part, new File(part.getPath() + ".chunks"), r.bytes);
                int percent = (int) (got * 100 / Math.max(1, r.bytes));
                showProgress(progress, bar, "已下载 " + percent + "%（"
                        + AppLog.bytes(got) + " / " + AppLog.bytes(r.bytes)
                        + "）\n再次开始会从断点继续");
            } else {
                showProgress(progress, bar, "尚未下载\n" + r.name + " · " + AppLog.bytes(r.bytes));
                bar.setProgress(0);
            }
        };
        refreshWaiting.run();

        /* What the pipeline is doing right now. One button starts it and one button stops it,
           wherever it has got to: checking, downloading, verifying, installing. */
        final String[] stage = {"idle"};

        /**
         * The whole path from "an image on Apple's server" to "macOS installed", in the order the
         * parts make sense in:
         *
         *   look for an image already here → verify it → (only if there is none, or it does not
         *   verify) download it → verify what arrived → install it
         *
         * Verifying before deciding is the point: an image that is already on the disk and passes
         * gets installed without a byte of network traffic, and one that does not pass is what
         * sends the pipeline to the download rather than the installer.
         */
        Runnable pipeline = () -> new Thread(() -> {
            PIPELINE.set(true);
            Releases.Release r = chosen[0];
            File downloaded = Releases.ipswFile(dir, r);
            String localPath = Prefs.localIpsw(a);
            File local = localPath == null ? null : new File(localPath);

            Runnable say = null;
            try {
                /* 1. What is already here, the user's own file first. */
                File source = null;
                boolean ours = false;
                if (local != null && local.exists() && local.length() > (1L << 30)) {
                    source = local;
                } else if (downloaded.exists() && downloaded.length() > 0) {
                    source = downloaded;
                    ours = true;
                }

                /* 2. If something is here, verify it before trusting it. */
                if (source != null) {
                    stage[0] = "verify";
                    File s0 = source;
                    boolean ok = verify(a, s0, progress, bar, cancelled);
                    if (cancelled.get()) return;
                    if (!ok) {
                        if (ours) {
                            /* A file this app fetched and that does not verify is ours to drop:
                               the verifier's sentence stays on screen, but the next stage is a
                               fresh download rather than the installer. */
                            a.runOnUiThread(() -> showProgress(progress, bar,
                                    "已下载的镜像校验不通过，已删除，将重新下载"));
                            s0.delete();
                            Prefs.clearVerified(a);
                            source = null;
                        } else {
                            /* The user's own file is theirs: it is not deleted. It is also not
                               installed — the pipeline goes on to the download, which is what the
                               flow says a failed verification leads to. */
                            a.runOnUiThread(() -> showProgress(progress, bar,
                                    "导入的镜像校验不通过，改为下载"));
                            AppLog.w("安装: 导入的镜像校验不通过，改为下载", null);
                            source = null;
                        }
                    }
                }

                /* 3. Nothing usable here: download, and wait for the download to end. */
                if (source == null) {
                    if (cancelled.get()) return;
                    stage[0] = "download";
                    AppLog.i("安装: 下载 " + r.name + "（" + AppLog.bytes(r.bytes) + "）");
                    Permissions.askForDownload(a);
                    a.runOnUiThread(() -> showProgress(progress, bar,
                            "开始下载 " + r.name + "\n" + AppLog.bytes(r.bytes)));
                    DownloadService.start(a, r);
                    while (!cancelled.get()) {
                        DownloadService.State st = DownloadService.state();
                        if (DownloadService.State.DONE.equals(st.phase)) break;
                        if (DownloadService.State.FAILED.equals(st.phase)
                                || DownloadService.State.CANCELLED.equals(st.phase)) {
                            /* The screen is already showing what the service said. */
                            return;
                        }
                        Thread.sleep(400);
                    }
                    if (cancelled.get()) {
                        DownloadService.cancel(a);
                        return;
                    }
                    source = downloaded;
                    ours = true;

                    /* 4. The second verification: what arrived, not what was expected. */
                    stage[0] = "verify";
                    a.runOnUiThread(() -> showProgress(progress, bar, "下载完成，校验中…"));
                    if (!verify(a, source, progress, bar, cancelled)) {
                        if (cancelled.get()) return;
                        source.delete();
                        Prefs.clearVerified(a);
                        a.runOnUiThread(() -> showProgress(progress, bar,
                                "下载的镜像校验不通过，已删除，请重试"));
                        return;
                    }
                }

                if (cancelled.get()) return;

                /* 5. Install what verified. */
                stage[0] = "install";
                a.runOnUiThread(() -> {
                    setAction(action, "停止安装");
                    showProgress(progress, bar, "准备安装…");
                });
                int run = InstallService.start(a);
                String outcome = install(a, dir, source, progress, cancelled);
                InstallService.stop(a, run);
                if (stage[0].equals("install")) stage[0] = "idle";
                final String o = outcome;
                a.runOnUiThread(() -> setAction(action, "下载并安装"));
                a.runOnUiThread(() -> {
                    if (o == null) showProgress(progress, bar, "安装完成。回到 Macs 页按「启动」。");
                    else if ("已取消".equals(o)) showProgress(progress, bar, "已停止。");
                    else showProgress(progress, bar, "安装失败: " + o);
                });
            } catch (Throwable t) {
                AppLog.w("安装: 流水线异常", t);
                a.runOnUiThread(() -> showProgress(progress, bar, "出错: " + t));
            } finally {
                stage[0] = "idle";
                PIPELINE.set(false);
            }
        }, "henge-pipeline").start();

        action.setOnClickListener(v -> {
            /* One button. While anything of the pipeline is running it stops it — the download,
               the verification or the install, whichever it has reached. */
            if (stage[0].equals("download") || DownloadService.running()) {
                cancelled.set(true);
                setAction(action, "正在停止…");
                DownloadService.cancel(a);
                return;
            }
            if (stage[0].equals("verify")) {
                cancelled.set(true);
                setAction(action, "正在停止…");
                return;
            }
            if (stage[0].equals("install") || InstallService.running()) {
                cancelled.set(true);
                setAction(action, "正在停止…");
                InstallService.requestStop(a);
                Native.nativeInstallStop();
                return;
            }
            cancelled.set(false);
            setAction(action, "停止");
            pipeline.run();
        });


        // The screen follows the service rather than the other way round: the download outlives
        // this view, so the view reads it instead of owning it.
        final Runnable[] self = new Runnable[1];
        final boolean[] everAttached = {false};
        Runnable follow = () -> a.runOnUiThread(() -> {
            /* A tab that was rebuilt (a reload, a rotation) must stop listening: its views are
               gone. Only after it has been on screen once, though: the first callback can arrive
               before the view is attached, and unsubscribing then left the screen dead. */
            if (progress.isAttachedToWindow()) {
                everAttached[0] = true;
            } else if (everAttached[0]) {
                if (self[0] != null) DownloadService.unlisten(self[0]);
                return;
            } else {
                return;
            }
            /* A pipeline started by another screen is still running: it owns the progress text,
               the bar and the button, so paint those and stop here. Without this the idle branch
               below repainted "nothing is happening" over a running install. */
            if (PIPELINE.get()) {
                if (lastText != null) progress.setText(lastText);
                if (lastBar >= 0) bar.setProgress(lastBar);
                if (lastAction != null) setAction(action, lastAction);
                return;
            }
            /* This screen is an Activity field, the service is not: after the app has been in the
               background the stage here is "idle" again while the download is still running, and
               the early return below then drew "not downloading" over a download that was going
               on. When nothing of our own is in flight, the service's state is the truth. */
            if ("idle".equals(stage[0])) {
                DownloadService.State now = DownloadService.state();
                boolean sameBuild = now.name == null || now.name.isEmpty()
                        || now.name.equals(chosen[0].build);
                /* Only a download that is actually running: the action button means "stop"
                   whenever this stage is "download", so adopting it for a finished download
                   turned the "安装" button into a cancel and the tap came back as a failure. */
                if (sameBuild && DownloadService.State.RUNNING.equals(now.phase)) {
                    stage[0] = "download";
                }
            }
            /* Download progress belongs to the screen only while the pipeline is in its download
               stage; in every other stage the pipeline itself says what is happening. */
            if (!"download".equals(stage[0])) return;
            DownloadService.State state = DownloadService.state();
            String phase = state.phase;
            /* The download service holds one state for the whole app, so a state that names a
               different build is the previous attempt's, not this one's. Showing it is what made
               "choose another version" look like it had already failed. */
            boolean mine = state.name == null || state.name.isEmpty()
                    || state.name.equals(chosen[0].build);
            if (!mine) phase = DownloadService.State.IDLE;
            if (DownloadService.State.RUNNING.equals(phase)) {
                setAction(action, "停止");
                int percent = state.total > 0 ? (int) (state.received * 100 / state.total) : 0;
                bar.setProgress(percent * 10);
                StringBuilder text = new StringBuilder("正在下载 ").append(state.name)
                        .append('\n')
                        .append(AppLog.bytes(state.received)).append(" / ")
                        .append(AppLog.bytes(state.total)).append("（").append(percent).append("%）");
                if (state.stalled) {
                    /* Frozen numbers with no explanation read as a frozen app; say what is known. */
                    text.append("\n网络无响应，正在重试…（已收到的部分保留）");
                } else if (state.speed > 0) {
                    text.append('\n').append(AppLog.bytes(state.speed)).append("/s");
                    if (state.etaMs > 0) text.append(" · 还需 ").append(AppLog.eta(state.etaMs));
                } else {
                    text.append("\n正在探测网速…");
                }
                progress.setText(text.toString());
            } else if (DownloadService.State.DONE.equals(phase)) {
                setAction(action, "安装");
                bar.setProgress(1000);
                progress.setText("已下载 " + state.name + "\n点「安装」开始写入磁盘");
            } else if (DownloadService.State.FAILED.equals(phase)) {
                setAction(action, "重试");
                String why = state.message == null ? "" : state.message.trim();
                if (why.isEmpty()) why = "原因未知，详情见设置里的日志";
                showProgress(progress, bar, "下载失败: " + why + "\n已下载的部分会保留，重试会从断点继续");
                bar.setProgress(1000 * (int) (state.total > 0 ? state.received * 1000 / state.total : 0) / 100);
            } else if (DownloadService.State.CANCELLED.equals(phase)) {
                setAction(action, "继续下载");
                progress.setText("已停止，已下载的部分会保留\n" + state.message);
            } else {
                setAction(action, "下载并安装");
                refreshWaiting.run();
            }
        });
        self[0] = follow;
        repaint[0] = follow;
        DownloadService.listen(follow);
        follow.run();

        /*
         * The screen also reads the state on its own, twice a second.
         *
         * Relying only on the service's callbacks means one lost or throttled callback freezes the
         * whole screen with the download running underneath, which is exactly how "it says
         * preparing and then nothing ever changes" happened. A timer cannot be lost.
         */
        final android.os.Handler ticker = new android.os.Handler(a.getMainLooper());
        Runnable[] tick = new Runnable[1];
        tick[0] = () -> {
            if (!progress.isAttachedToWindow()) return;   /* the view is gone: stop the timer */
            follow.run();
            ticker.postDelayed(tick[0], 500);
        };
        ticker.postDelayed(tick[0], 500);


        /* The button label lives outside the view as well, so a screen built while a pipeline is
           running shows "stop" rather than "download and install". */
        if (PIPELINE.get() && lastAction != null) setAction(action, lastAction);

        return root;
    }

    /**
     * Verifies `image` unless it was already verified, and says so on screen while it does.
     *
     * Returns false when the image does not verify or the user stopped it; the caller decides what
     * that means — drop the file and download, or stop and say why.
     */
    private static boolean verify(Activity a, File image, TextView progress,
                                  android.widget.ProgressBar bar, AtomicBoolean cancelled) {
        if (Prefs.isVerified(a, image)) {
            AppLog.i("校验: 已校验过，跳过（" + image.getName() + "）");
            a.runOnUiThread(() -> showProgress(progress, bar,
                    "镜像已校验过\n" + image.getName()));
            return !cancelled.get();
        }
        a.runOnUiThread(() -> showProgress(progress, bar, "校验镜像…\n" + image.getName()));
        IpswIntegrity.Result check = IpswIntegrity.verify(image,
                line -> a.runOnUiThread(() -> showProgress(progress, bar, line)), cancelled);
        if (check == null) return false;                 /* stopped */
        if (!check.ok) {
            AppLog.w("校验: 失败 " + check.problem, null);
            return false;
        }
        Prefs.markVerified(a, image);
        AppLog.i("校验: 通过 " + image.getName());
        return true;
    }

    /** The percentage inside a status line, or 0 when it carries none. */
    private static int percentOf(String text) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(\\d{1,3})%").matcher(text);
        return m.find() ? Integer.parseInt(m.group(1)) : 0;
    }

    /** Sets the state line and, when it carries a percentage, the bar under it. */
    /* The last thing this screen said. The pipeline runs on a thread of its own and keeps writing
       through views that a screen change has already thrown away, so the message is kept here as
       well and a rebuilt screen paints it again -- otherwise switching away and back looked like
       the install had stopped. */
    private static volatile String lastText;
    private static volatile int lastBar = -1;
    private static volatile String lastAction;
    /* The pipeline outlives the screen that started it: it is a thread of the app, not of this
       view. While it runs, it owns what the screen says, and a rebuilt screen renders from these
       statics instead of falling through to "nothing is happening". */
    private static final java.util.concurrent.atomic.AtomicBoolean PIPELINE =
            new java.util.concurrent.atomic.AtomicBoolean(false);

    /* One place that sets the button, so its label survives a screen change too. */
    private static void setAction(android.widget.Button action, String label) {
        lastAction = label;
        setAction(action, label);
    }

    private static void showProgress(TextView text, android.widget.ProgressBar bar, String message) {
        lastText = message;
        text.setText(message);
        java.util.regex.Matcher m = java.util.regex.Pattern
                .compile("(\\d{1,3})%").matcher(message);
        if (m.find()) {
            bar.setProgress(Math.min(1000, Integer.parseInt(m.group(1)) * 10));
        } else if (message.contains("完成")) {
            bar.setProgress(1000);
        }
        lastBar = bar.getProgress();
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

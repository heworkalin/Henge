package dev.henge.probe;

import java.io.File;

/**
 * The macOS restore images Apple publishes for virtual Macs (VirtualMac2,1).
 *
 * These are the same builds the iOS app offers, from Apple's own CDN. They are listed rather than
 * discovered: Apple's update catalog (mesu.apple.com) only advertises the newest build, and this
 * machine's GPU path was brought up against specific ones — offering a build nobody has booted here
 * would be offering a coin toss. Every entry below is signed by Apple for virtual Macs.
 *
 * Order is oldest first, as on iOS: the small ones matter on a device that has little room, and the
 * newest is not always the one that runs best here.
 */
final class Releases {

    static final class Release {
        final String build;
        final String name;
        final String url;
        /** Size of the download, from Apple's own listing; used for the progress bar before the
         *  server says otherwise, and to decide whether a part file is finished. */
        final long bytes;
        final String note;
        /** Whether this build has actually been booted to the end of an install on these devices.
         *  Only one has: the rest are offered so they can be tried, and are labelled as such, because
         *  a build nobody has run here is a coin toss and the UI should not pretend otherwise. */
        final boolean tested;

        Release(String build, String name, String url, long bytes, boolean tested, String note) {
            this.build = build;
            this.name = name;
            this.url = url;
            this.bytes = bytes;
            this.tested = tested;
            this.note = note;
        }

        @Override
        public String toString() {
            return name + " (" + build + ")";
        }
    }

    private static final long MB = 1L << 20;

    static final Release[] ALL = {
            new Release("22G120", "macOS Ventura 13.6",
                    "https://updates.cdn-apple.com/2023FallFCS/fullrestores/042-55833/C0830847-A2F8-458F-B680-967991820931/UniversalMac_13.6_22G120_Restore.ipsw",
                    12_297 * MB, false, "体积最小，装完约 35 GB。"),
            new Release("23G93", "macOS Sonoma 14.6.1",
                    "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw",
                    14_106 * MB, false, "比 Sequoia 小一些，装完约 40 GB。"),
            new Release("24G90", "macOS Sequoia 15.6.1",
                    "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw",
                    16_035 * MB, true, "唯一验证过的版本，安装流程就按它调试。"),
            new Release("25G83", "macOS Tahoe 26.6.2",
                    "https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw",
                    18_856 * MB, false, "首次设置较慢（建账号可能要十分钟）。"),
            new Release("26B123", "macOS 27.2",
                    "https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw",
                    24_409 * MB, false, "最新，也最慢；Safari 的页面可能留白。"),
    };

    /** Offered when nothing has been downloaded yet: the build this port is tested against. */
    static final Release RECOMMENDED = byBuild("24G90");

    private Releases() {
    }

    static Release byBuild(String build) {
        for (Release r : ALL) if (r.build.equals(build)) return r;
        return ALL[0];
    }

    /** Where a release's download lives while it is in progress and once it is done. */
    static File partFile(File dir, Release r) {
        return new File(dir, r.build + ".ipsw.part");
    }

    static File ipswFile(File dir, Release r) {
        return new File(dir, r.build + ".ipsw");
    }
}

//
// gdbwrap — the entry point of a self-contained GDB bundle.
//
// Why this exists. The old launcher was a shell script with the Termux prefix
// hard-coded:
//
//     P=/data/local/tmp/termux/files/usr
//     export LD_LIBRARY_PATH=$P/lib
//     export GUILE_LOAD_PATH=$P/share/guile/site:...
//     exec $P/bin/gdb "$@"
//
// It broke for two reasons that have nothing to do with gdb: the prefix is a
// fixed path (a package update or a move makes it wrong), and every argument
// crosses a shell, so a register dump or a gdb command with a quote, `$`, `;`,
// `!` or a glob is re-interpreted before gdb ever sees it.
//
// This binary is the fix. It takes its own directory as the prefix, so the
// bundle can be tarred, pushed anywhere and run; and it execs gdb directly, so
// arguments reach it byte for byte.
//
// Layout it expects (it lives in bin/, next to the real binary):
//
//     <bundle>/bin/gdb        this binary
//     <bundle>/bin/gdb.real   the gdb it launches
//     <bundle>/lib/*.so       that gdb's whole dependency tree
//     <bundle>/share/guile/   guile's Scheme sources, without which gdb aborts
//     <bundle>/lib/guile/     guile's compiled cache
//
// It links nothing but the system libc on purpose: this program has to run even
// when the bundle's own libraries are the thing that is broken.
//
// Build (see tools/build-gdb-bundle.sh):
//     aarch64-linux-android28-clang -O2 -Wall -Wextra -o gdb tools/gdbwrap.c
//

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The bundle's root: the parent of the directory this binary is in.
//
// /proc/self/exe is the one path that stays true after the bundle has been
// moved, renamed or reached through a symlink; argv[0] is not, because a shell
// may pass a bare name and a symlink may point anywhere.
static int bundle_root(char *buf, size_t cap)
{
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1)
        return -1;
    buf[n] = '\0';

    // <root>/bin/gdb -> <root>
    char *slash = strrchr(buf, '/');
    if (slash == NULL)
        return -1;
    *slash = '\0';
    slash = strrchr(buf, '/');
    if (slash == NULL || slash == buf)
        return -1;
    *slash = '\0';
    return 0;
}

// setenv, but a failure is fatal: a half-set environment is the abort this
// program exists to prevent.
static void must_setenv(const char *name, const char *value)
{
    if (setenv(name, value, 1) != 0) {
        fprintf(stderr, "gdbwrap: setenv(%s) failed\n", name);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    char root[PATH_MAX];
    if (bundle_root(root, sizeof root) != 0) {
        fprintf(stderr, "gdbwrap: cannot resolve my own directory\n");
        return 1;
    }

    char path[PATH_MAX], value[8 * PATH_MAX];

    // The bundle's libraries first, then whatever the caller had.
    const char *old_ld = getenv("LD_LIBRARY_PATH");
    snprintf(value, sizeof value, "%s/lib%s%s", root,
             (old_ld && *old_ld) ? ":" : "", (old_ld && *old_ld) ? old_ld : "");
    must_setenv("LD_LIBRARY_PATH", value);

    // The three guile variables. All of them are required: with the Scheme
    // sources missing (the first) gdb dies in scm_load_startup_files before
    // main(), with an abort and no message.
    snprintf(value, sizeof value, "%s/share/guile/site:%s/share/guile/site/3.0", root, root);
    must_setenv("GUILE_LOAD_PATH", value);

    snprintf(value, sizeof value, "%s/lib/guile/3.0/ccache:%s/share/guile/site/3.0", root, root);
    must_setenv("GUILE_LOAD_COMPILED_PATH", value);

    // The commands gdb runs (objdump, readelf, ...) would come from here too,
    // which is why the bundle's bin comes first; the bundle carries none of
    // them, gdb's own disassembler does not need them.
    const char *old_path = getenv("PATH");
    snprintf(value, sizeof value, "%s/bin%s%s", root,
             (old_path && *old_path) ? ":" : "", (old_path && *old_path) ? old_path : "");
    must_setenv("PATH", value);

    // Python: libpython finds its standard library at a path compiled into
    // gdb (/data/data/com.termux/files/usr/lib/python3.14), which does not
    // exist here. PYTHONHOME re-roots it at the bundle: CPython then looks for
    // $PYTHONHOME/lib/python3.14.
    must_setenv("PYTHONHOME", root);

    snprintf(path, sizeof path, "%s/bin/gdb.real", root);

    // gdb's data directory -- guile's gdb modules (boot.scm), the Python
    // support library, the syscall tables -- is likewise a compile-time path.
    // --data-directory is the only supported override, and it goes first so a
    // command line in a shell wrapper cannot lose it; an explicit one from the
    // caller wins.
    char datadir[PATH_MAX + 32];
    snprintf(datadir, sizeof datadir, "--data-directory=%s/share/gdb", root);
    int have_datadir = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--data-directory", 16) == 0)
            have_datadir = 1;
    }
    if (!have_datadir) {
        char **shifted = malloc(sizeof *shifted * (size_t)(argc + 2));
        if (shifted == NULL) {
            fprintf(stderr, "gdbwrap: out of memory\n");
            return 1;
        }
        shifted[0] = argv[0];
        shifted[1] = datadir;
        for (int i = 1; i < argc; i++)
            shifted[i + 1] = argv[i];
        shifted[argc + 1] = NULL;
        argv = shifted;
    }

    execv(path, argv);

    // Only reachable when the bundle is incomplete.
    fprintf(stderr, "gdbwrap: cannot exec %s\n", path);
    return 127;
}

// glibcwrapper.c
//
// Wraps a glibc-linked ELF binary bundled as a jniLibs .so, executing it
// via glibc's own dynamic loader (bundled alongside as another .so),
// bypassing the kernel's PT_INTERP resolution entirely (which fails
// because /lib/ld-linux-aarch64.so.1 doesn't exist on Android).
//
// This binary is placed in jniLibs as "libimpellerc.so" (or similar).
// The original glibc binary is renamed to "<name>.so.real" alongside it.
// The glibc dynamic loader itself is bundled as "libglibc_ld.so", and all
// DT_NEEDED entries in both the target binary and its glibc deps are
// patched (via patchelf) to bare filenames (e.g. "libglibc_c.so") so the
// loader resolves them via LD_LIBRARY_PATH, set below to our own dir --
// making this immune to nativeLibraryDir's hash changing on every
// reinstall (no absolute paths baked in anywhere).
//
// At runtime:
//   1. Resolve our own real path via /proc/self/exe -> gives the jniLibs
//      dir (nativeLibraryDir), which also contains libglibc_ld.so and
//      the glibc dependency .so files.
//   2. Set LD_LIBRARY_PATH to that dir so the glibc loader can find its
//      needed libraries by bare name.
//   3. execv() the glibc loader itself, passing "<argv[0]>.real" as the
//      program to load, forwarding the rest of argv.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    char self_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len == -1) {
        fprintf(stderr, "glibcwrapper: readlink failed: %s\n", strerror(errno));
        return 127;
    }
    self_path[len] = '\0';

    char dir_buf[PATH_MAX];
    strncpy(dir_buf, self_path, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    char *libdir = dirname(dir_buf);

    char base_buf[PATH_MAX];
    strncpy(base_buf, self_path, sizeof(base_buf) - 1);
    base_buf[sizeof(base_buf) - 1] = '\0';
    char *base = basename(base_buf);

    char real_target[PATH_MAX];
    if (snprintf(real_target, sizeof(real_target), "%s/%s.real", libdir, base) >= (int)sizeof(real_target)) {
        fprintf(stderr, "glibcwrapper: path too long\n");
        return 127;
    }

    char loader[PATH_MAX];
    if (snprintf(loader, sizeof(loader), "%s/libglibc_ld.so", libdir) >= (int)sizeof(loader)) {
        fprintf(stderr, "glibcwrapper: loader path too long\n");
        return 127;
    }

    char ld_path_env[PATH_MAX + 32];
    snprintf(ld_path_env, sizeof(ld_path_env), "LD_LIBRARY_PATH=%s", libdir);
    putenv(ld_path_env);

    char **new_argv = malloc(sizeof(char *) * (size_t)(argc + 2));
    if (new_argv == NULL) {
        fprintf(stderr, "glibcwrapper: out of memory\n");
        return 127;
    }
    new_argv[0] = loader;
    new_argv[1] = real_target;
    for (int i = 1; i < argc; i++) new_argv[i + 1] = argv[i];
    new_argv[argc + 1] = NULL;

    execv(loader, new_argv);
    fprintf(stderr, "glibcwrapper: execv(%s) failed: %s\n", loader, strerror(errno));
    free(new_argv);
    return 127;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>

static void write_debug_log(const char *msg) {
    FILE *f = fopen("/data/data/com.kurd.apkapp.kurdapk/app_flutter/impellerc_crash_debug.txt", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    } else {
        // Also try stderr in case terminal captures it
        fprintf(stderr, "LOGFAIL: %s (errno=%d %s)\n", msg, errno, strerror(errno));
    }
}

int main(int argc, char *argv[]) {
    write_debug_log("STEP1: main() entered");

    char self_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len == -1) { fprintf(stderr, "glibcwrapper: readlink failed: %s\n", strerror(errno)); return 127; }
    self_path[len] = '\0';
    write_debug_log("STEP2: readlink done");

    char dir_buf[PATH_MAX];
    strncpy(dir_buf, self_path, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    char *libdir = dirname(dir_buf);
    write_debug_log("STEP3: dirname done");

    char base_buf[PATH_MAX];
    strncpy(base_buf, self_path, sizeof(base_buf) - 1);
    base_buf[sizeof(base_buf) - 1] = '\0';
    char *base = basename(base_buf);
    write_debug_log("STEP4: basename done");

    char base_noext[PATH_MAX];
    strncpy(base_noext, base, sizeof(base_noext) - 1);
    base_noext[sizeof(base_noext) - 1] = '\0';
    size_t blen = strlen(base_noext);
    if (blen > 3 && strcmp(base_noext + blen - 3, ".so") == 0) {
        base_noext[blen - 3] = '\0';
    }

    char real_target[PATH_MAX];
    snprintf(real_target, sizeof(real_target), "%s/%s__real.so", libdir, base_noext);

    char loader[PATH_MAX];
    snprintf(loader, sizeof(loader), "%s/libglibc_ld.so", libdir);
    write_debug_log("STEP5: paths built");

    unsetenv("LD_PRELOAD");
    char ld_path_env[PATH_MAX + 32];
    snprintf(ld_path_env, sizeof(ld_path_env), "LD_LIBRARY_PATH=%s", libdir);
    putenv(ld_path_env);
    write_debug_log("STEP6: env set");

    char **new_argv = malloc(sizeof(char *) * (size_t)(argc + 4));
    if (new_argv == NULL) { fprintf(stderr, "glibcwrapper: out of memory\n"); return 127; }
    new_argv[0] = loader;
    new_argv[1] = "--library-path";
    new_argv[2] = libdir;
    new_argv[3] = real_target;
    for (int i = 1; i < argc; i++) new_argv[i + 3] = argv[i];
    new_argv[argc + 3] = NULL;
    write_debug_log("STEP7: argv built, about to execv");

    execv(loader, new_argv);
    write_debug_log("STEP8: execv RETURNED (this means execv FAILED)");
    fprintf(stderr, "glibcwrapper: execv(%s) failed: %s\n", loader, strerror(errno));
    free(new_argv);
    return 127;
}

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
    if (len == -1) { fprintf(stderr, "glibcwrapper: readlink failed: %s\n", strerror(errno)); return 127; }
    self_path[len] = '\0';

    char dir_buf[PATH_MAX];
    strncpy(dir_buf, self_path, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    char *libdir = dirname(dir_buf);

    char base_buf[PATH_MAX];
    strncpy(base_buf, self_path, sizeof(base_buf) - 1);
    base_buf[sizeof(base_buf) - 1] = '\0';
    char *base = basename(base_buf);

    // Strip trailing ".so" and append "__real.so" (AGP drops jniLibs
    // files not ending in exactly ".so", so we can't just append ".real")
    char base_noext[PATH_MAX];
    strncpy(base_noext, base, sizeof(base_noext) - 1);
    base_noext[sizeof(base_noext) - 1] = '\0';
    size_t blen = strlen(base_noext);
    if (blen > 3 && strcmp(base_noext + blen - 3, ".so") == 0) {
        base_noext[blen - 3] = '\0';
    }

    char real_target[PATH_MAX];
    if (snprintf(real_target, sizeof(real_target), "%s/%s__real.so", libdir, base_noext) >= (int)sizeof(real_target)) {
        fprintf(stderr, "glibcwrapper: path too long\n"); return 127;
    }

    char loader[PATH_MAX];
    if (snprintf(loader, sizeof(loader), "%s/libglibc_ld.so", libdir) >= (int)sizeof(loader)) {
        fprintf(stderr, "glibcwrapper: loader path too long\n"); return 127;
    }

    unsetenv("LD_PRELOAD");
    char ld_path_env[PATH_MAX + 32];
    snprintf(ld_path_env, sizeof(ld_path_env), "LD_LIBRARY_PATH=%s", libdir);
    putenv(ld_path_env);

    char **new_argv = malloc(sizeof(char *) * (size_t)(argc + 4));
    if (new_argv == NULL) { fprintf(stderr, "glibcwrapper: out of memory\n"); return 127; }
    new_argv[0] = loader;
    new_argv[1] = "--library-path";
    new_argv[2] = libdir;
    new_argv[3] = real_target;
    for (int i = 1; i < argc; i++) new_argv[i + 3] = argv[i];
    new_argv[argc + 3] = NULL;

    execv(loader, new_argv);
    fprintf(stderr, "glibcwrapper: execv(%s) failed: %s\n", loader, strerror(errno));
    free(new_argv);
    return 127;
}

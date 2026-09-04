// Wrapper for NDK's clang: injects the flags bin_clang-21.so needs
// (resource-dir, -B, -L for lld/libunwind) and sets LD_LIBRARY_PATH,
// then execve's the real bionic clang. Placed in jniLibs so it's
// exec-permitted; NDK's own toolchains/.../bin/clang is replaced
// with a symlink pointing at this wrapper.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>

extern char **environ;

int main(int argc, char *argv[]) {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) { perror("readlink /proc/self/exe"); return 127; }
    exe_path[len] = '\0';

    char *exec_dir = strdup(dirname(exe_path));

    const char *data_dir = getenv("KURDAPK_CLANG_DATA_DIR");
    if (!data_dir) {
        fprintf(stderr, "clangwrapper: KURDAPK_CLANG_DATA_DIR not set\n");
        return 127;
    }

    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%s/bin_clang-21.so", exec_dir);

    char resource_dir_flag[PATH_MAX];
    snprintf(resource_dir_flag, sizeof(resource_dir_flag),
             "-resource-dir=%s/lib/clang/21", data_dir);

    char b_flag[PATH_MAX];
    snprintf(b_flag, sizeof(b_flag), "-B%s", exec_dir);

    char l_flag[PATH_MAX];
    snprintf(l_flag, sizeof(l_flag), "-L%s/lib", data_dir);

    setenv("LD_LIBRARY_PATH", exec_dir, 1);

    int new_argc = argc + 3;
    char **new_argv = malloc((new_argc + 1) * sizeof(char *));
    int i = 0;
    new_argv[i++] = target;
    new_argv[i++] = resource_dir_flag;
    new_argv[i++] = b_flag;
    new_argv[i++] = l_flag;
    for (int j = 1; j < argc; j++) new_argv[i++] = argv[j];
    new_argv[i] = NULL;

    execve(target, new_argv, environ);
    perror("clangwrapper: execve failed");
    return 127;
}

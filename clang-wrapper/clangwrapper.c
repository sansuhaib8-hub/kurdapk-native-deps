// Wrapper for NDK's clang: injects the flags bin_clang-21.so needs
// (resource-dir, -B, -L for lld/libunwind, and per-API crt dir),
// then execve's the real bionic clang. Placed in jniLibs so it's
// exec-permitted; NDK's own toolchains/.../bin/clang is replaced
// with a symlink pointing at this wrapper.
//
// Note: cmake's Android toolchain file can fail to detect the host
// OS tag when running on-device (uname reports Android/aarch64, not
// linux-x86_64), producing a --sysroot= value like
// ".../prebuilt//sysroot" (missing the "linux-x86_64" host segment).
// The actual crtbegin/crtend objects only exist under
// ".../prebuilt/linux-x86_64/sysroot/...", so we probe for that and
// fall back to inserting "linux-x86_64" if the naive path is missing.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>

extern char **environ;

static const char *multiarch_for(const char *arch) {
    if (strcmp(arch, "aarch64") == 0) return "aarch64-linux-android";
    if (strncmp(arch, "arm", 3) == 0) return "arm-linux-androideabi";
    if (strcmp(arch, "i686") == 0) return "i686-linux-android";
    if (strcmp(arch, "x86_64") == 0) return "x86_64-linux-android";
    return NULL;
}

/* Strip a trailing "/sysroot" (with any trailing slashes before it)
   from sysroot_arg, writing the toolchain root into out. */
static void toolchain_root_from_sysroot(const char *sysroot_arg, char *out, size_t out_sz) {
    size_t len = strlen(sysroot_arg);
    while (len > 0 && sysroot_arg[len - 1] == '/') len--;
    const char *suffix = "/sysroot";
    size_t suffix_len = strlen(suffix);
    if (len >= suffix_len && strncmp(sysroot_arg + len - suffix_len, suffix, suffix_len) == 0) {
        len -= suffix_len;
    }
    while (len > 0 && sysroot_arg[len - 1] == '/') len--;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, sysroot_arg, len);
    out[len] = '\0';
}

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

    /* Scan argv for --sysroot= and --target= to compute the versioned
       crt directory (e.g. <sysroot>/usr/lib/aarch64-linux-android/21)
       that bin_clang-21.so can't auto-detect on its own. */
    const char *sysroot_arg = NULL;
    const char *target_arg = NULL;
    for (int j = 1; j < argc; j++) {
        if (strncmp(argv[j], "--sysroot=", 10) == 0) sysroot_arg = argv[j] + 10;
        else if (strncmp(argv[j], "--target=", 9) == 0) target_arg = argv[j] + 9;
    }

    char crt_b_flag[PATH_MAX] = {0};
    if (sysroot_arg && target_arg) {
        char arch[32] = {0};
        const char *dash = strchr(target_arg, '-');
        size_t arch_len = dash ? (size_t)(dash - target_arg) : strlen(target_arg);
        if (arch_len >= sizeof(arch)) arch_len = sizeof(arch) - 1;
        memcpy(arch, target_arg, arch_len);
        arch[arch_len] = '\0';

        int tlen = (int)strlen(target_arg);
        int k = tlen;
        while (k > 0 && target_arg[k-1] >= '0' && target_arg[k-1] <= '9') k--;
        const char *api = (k < tlen) ? target_arg + k : "21";

        const char *multiarch = multiarch_for(arch);
        if (multiarch) {
            /* Candidate 1: sysroot_arg as given by cmake, unmodified. */
            char candidate1[PATH_MAX];
            snprintf(candidate1, sizeof(candidate1), "%s/usr/lib/%s/%s", sysroot_arg, multiarch, api);
            char probe1[PATH_MAX];
            snprintf(probe1, sizeof(probe1), "%s/crtbegin_dynamic.o", candidate1);

            if (access(probe1, F_OK) == 0) {
                snprintf(crt_b_flag, sizeof(crt_b_flag), "-B%s", candidate1);
            } else {
                /* Candidate 2: insert linux-x86_64 host segment before
                   the trailing "sysroot" component (handles the
                   on-device host-tag-detection-failure case). */
                char toolchain_root[PATH_MAX];
                toolchain_root_from_sysroot(sysroot_arg, toolchain_root, sizeof(toolchain_root));
                char candidate2[PATH_MAX];
                snprintf(candidate2, sizeof(candidate2), "%s/linux-x86_64/sysroot/usr/lib/%s/%s",
                         toolchain_root, multiarch, api);
                char probe2[PATH_MAX];
                snprintf(probe2, sizeof(probe2), "%s/crtbegin_dynamic.o", candidate2);

                if (access(probe2, F_OK) == 0) {
                    snprintf(crt_b_flag, sizeof(crt_b_flag), "-B%s", candidate2);
                } else {
                    /* Neither candidate exists; fall back to candidate1
                       so clang's own error message stays informative. */
                    snprintf(crt_b_flag, sizeof(crt_b_flag), "-B%s", candidate1);
                }
            }
        }
    }

    setenv("LD_LIBRARY_PATH", exec_dir, 1);

    int have_crt_flag = crt_b_flag[0] != '\0';
    int new_argc = argc + 3 + (have_crt_flag ? 1 : 0);
    char **new_argv = malloc((new_argc + 1) * sizeof(char *));
    int i = 0;
    new_argv[i++] = target;
    new_argv[i++] = resource_dir_flag;
    new_argv[i++] = b_flag;
    new_argv[i++] = l_flag;
    if (have_crt_flag) new_argv[i++] = crt_b_flag;
    for (int j = 1; j < argc; j++) new_argv[i++] = argv[j];
    new_argv[i] = NULL;

    if (getenv("KURDAPK_WRAPPER_DEBUG")) {
        fprintf(stderr, "[wrapper] argc=%d\n", new_argc);
        for (int d = 0; d < new_argc; d++) fprintf(stderr, "[wrapper]   argv[%d]=%s\n", d, new_argv[d]);
    }

    execve(target, new_argv, environ);
    perror("clangwrapper: execve failed");
    return 127;
}

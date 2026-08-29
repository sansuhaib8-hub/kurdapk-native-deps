// execwrapper.c
//
// Generic script-exec wrapper for Android's W^X policy.
//
// Problem: execve() of any file under app-private storage (app_flutter/)
// is blocked by SELinux, even for a script with a correct shebang. Only
// binaries pre-packaged in jniLibs (and extracted by the OS into the
// exec-permitted nativeLibraryDir) may be exec'd directly.
//
// This binary is placed in jniLibs as "libexecwrapper.so", and a symlink
// is created at the *original* location of any script that needs to be
// runnable (e.g. usr/opt/flutter/bin/flutter), pointing to this .so.
// The original script is renamed to "<name>.real" and left in place.
//
// At runtime:
//   1. Resolve our own real path via /proc/self/exe -> gives the jniLibs
//      dir, which also contains bin_bash.so.
//   2. argv[0] is preserved by the kernel as the invoked (symlink) path.
//      Append ".real" to find the original script.
//   3. execve() bash with the real script as argv[1], forwarding the
//      rest of argv and the full environment.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>

int main(int argc, char *argv[], char *envp[]) {
    char self_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len == -1) {
        fprintf(stderr, "execwrapper: failed to readlink /proc/self/exe: %s\n",
                strerror(errno));
        return 127;
    }
    self_path[len] = '\0';

    char self_path_copy[PATH_MAX];
    strncpy(self_path_copy, self_path, sizeof(self_path_copy) - 1);
    self_path_copy[sizeof(self_path_copy) - 1] = '\0';
    char *lib_dir = dirname(self_path_copy);

    char bash_path[PATH_MAX];
    int n = snprintf(bash_path, sizeof(bash_path), "%s/bin_bash.so", lib_dir);
    if (n < 0 || (size_t)n >= sizeof(bash_path)) {
        fprintf(stderr, "execwrapper: bash path too long\n");
        return 127;
    }

    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "execwrapper: argv[0] missing, cannot locate target script\n");
        return 127;
    }

    char real_script[PATH_MAX];
    n = snprintf(real_script, sizeof(real_script), "%s.real", argv[0]);
    if (n < 0 || (size_t)n >= sizeof(real_script)) {
        fprintf(stderr, "execwrapper: script path too long\n");
        return 127;
    }

    char **new_argv = malloc(sizeof(char *) * (size_t)(argc + 2));
    if (new_argv == NULL) {
        fprintf(stderr, "execwrapper: out of memory\n");
        return 127;
    }
    new_argv[0] = bash_path;
    new_argv[1] = real_script;
    for (int i = 1; i < argc; i++) {
        new_argv[i + 1] = argv[i];
    }
    new_argv[argc + 1] = NULL;

    execve(bash_path, new_argv, envp);

    fprintf(stderr, "execwrapper: execve(%s) failed: %s\n", bash_path, strerror(errno));
    free(new_argv);
    return 127;
}

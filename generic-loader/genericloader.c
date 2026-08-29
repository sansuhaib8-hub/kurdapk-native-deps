// genericloader.c
//
// Generic exec loader for Android's noexec app_flutter restriction.
// Unlike execwrapper.c (which re-execs bash with a script path, only
// works for shell scripts), this loads the target file's raw bytes into
// an anonymous memfd (which has no filesystem/mount, so Android's W^X
// noexec policy on app_flutter never applies to it) and executes THAT
// memfd directly via fexecve(). This works for both ELF binaries and
// shebang scripts, since the kernel's normal exec machinery (including
// shebang interpretation) operates on the memfd like any other file.
//
// Convention: same as execwrapper.c. A symlink is placed at the
// original location of a binary that needs to run (e.g.
// .../engine/android-arm64-release/linux-arm64/gen_snapshot), pointing
// to this .so. The original binary is renamed to "<name>.real" and left
// in place. argv[0] (preserved by the kernel as the invoked path) is
// used, with ".real" appended, to find the real target.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

int main(int argc, char *argv[], char *envp[]) {
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        fprintf(stderr, "genericloader: argv[0] missing, cannot locate target\n");
        return 127;
    }

    char real_path[PATH_MAX];
    int n = snprintf(real_path, sizeof(real_path), "%s.real", argv[0]);
    if (n < 0 || (size_t)n >= sizeof(real_path)) {
        fprintf(stderr, "genericloader: target path too long\n");
        return 127;
    }

    int src_fd = open(real_path, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "genericloader: failed to open %s: %s\n", real_path, strerror(errno));
        return 127;
    }

    long memfd = syscall(SYS_memfd_create, "genericloader_exec", 0);
    if (memfd < 0) {
        fprintf(stderr, "genericloader: memfd_create failed: %s\n", strerror(errno));
        close(src_fd);
        return 127;
    }

    char buf[65536];
    ssize_t r;
    while ((r = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write((int)memfd, buf + off, (size_t)(r - off));
            if (w < 0) {
                fprintf(stderr, "genericloader: write to memfd failed: %s\n", strerror(errno));
                close(src_fd);
                close((int)memfd);
                return 127;
            }
            off += w;
        }
    }
    if (r < 0) {
        fprintf(stderr, "genericloader: read %s failed: %s\n", real_path, strerror(errno));
        close(src_fd);
        close((int)memfd);
        return 127;
    }
    close(src_fd);

    // Preserve original argv as-is (argv[0] stays the invoked symlink
    // path so any target that inspects its own name still sees the
    // expected value). Bionic (Android's libc) has no fexecve() wrapper,
    // so call the underlying execveat() syscall directly with an empty
    // path and AT_EMPTY_PATH, which is exactly what fexecve() does on
    // glibc under the hood.
    syscall(SYS_execveat, (int)memfd, "", argv, envp, AT_EMPTY_PATH);

    fprintf(stderr, "genericloader: execveat failed: %s\n", strerror(errno));
    close((int)memfd);
    return 127;
}

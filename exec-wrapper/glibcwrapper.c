#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>

static void write_debug_log(const char *msg) {
    FILE *f = fopen("/sdcard/impellerc_crash_debug.txt", "a");
    if (f) {
        time_t t = time(NULL);
        fprintf(f, "[%ld] %s\n", (long)t, msg);
        fclose(f);
    }
}

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

    char logmsg[512];
    snprintf(logmsg, sizeof(logmsg), "About to fork+exec: loader=%s target=%s libdir=%s", loader, real_target, libdir);
    write_debug_log(logmsg);

    pid_t pid = fork();
    if (pid == -1) {
        write_debug_log("fork() failed");
        fprintf(stderr, "glibcwrapper: fork failed: %s\n", strerror(errno));
        free(new_argv);
        return 127;
    }

    if (pid == 0) {
        execv(loader, new_argv);
        write_debug_log("execv failed in child");
        fprintf(stderr, "glibcwrapper: execv(%s) failed: %s\n", loader, strerror(errno));
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        snprintf(logmsg, sizeof(logmsg), "Child exited normally, code=%d", WEXITSTATUS(status));
        write_debug_log(logmsg);
        free(new_argv);
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        snprintf(logmsg, sizeof(logmsg), "Child KILLED by signal %d (%s)%s", sig, strsignal(sig),
                 WCOREDUMP(status) ? " [core dumped]" : " [no core]");
        write_debug_log(logmsg);
        free(new_argv);
        return 128 + sig;
    } else {
        write_debug_log("Child died with unknown status");
        free(new_argv);
        return 127;
    }
}

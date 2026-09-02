#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <malloc.h>
#include <libgen.h>
#include "kurdapk_javac_class.h"

typedef jint (JNICALL *CreateJavaVM_t)(JavaVM **, void **, void *);

static void dots_to_slashes(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == '.') *p = '/';
    }
}

static int load_predeps_and_jvm(const char *libdir, void **out_jvm_handle) {
    char path_buf[1024];
    const char *predeps[] = {"libandroid-shmem.so", "libandroid-spawn.so", NULL};
    for (int i = 0; predeps[i] != NULL; i++) {
        snprintf(path_buf, sizeof(path_buf), "%s/%s", libdir, predeps[i]);
        void *h = dlopen(path_buf, RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            fprintf(stderr, "dlopen %s failed: %s\n", predeps[i], dlerror());
            return 1;
        }
    }
    char libjvm_path[1024];
    snprintf(libjvm_path, sizeof(libjvm_path), "%s/server/libjvm.so", libdir);
    void *jvm_handle = dlopen(libjvm_path, RTLD_NOW | RTLD_GLOBAL);
    if (!jvm_handle) {
        fprintf(stderr, "dlopen libjvm.so failed: %s\n", dlerror());
        return 1;
    }
    *out_jvm_handle = jvm_handle;
    return 0;
}

static int run_kurdapk_javac(JNIEnv *env, JavaVM *jvm, int argc_extra, char **argv_extra) {
    jclass cls = (*env)->DefineClass(env, "KurdApkJavac", NULL,
                                      (const jbyte *)KurdApkJavac_class,
                                      (jsize)KurdApkJavac_class_len);
    if (cls == NULL) {
        fprintf(stderr, "Could not define KurdApkJavac class\n");
        (*env)->ExceptionDescribe(env);
        (*jvm)->DestroyJavaVM(jvm);
        return 1;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "main", "([Ljava/lang/String;)V");
    if (mid == NULL) {
        fprintf(stderr, "Could not find KurdApkJavac.main\n");
        (*env)->ExceptionDescribe(env);
        (*jvm)->DestroyJavaVM(jvm);
        return 1;
    }

    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    jobjectArray mainArgs = (*env)->NewObjectArray(env, argc_extra > 0 ? argc_extra : 0, stringClass, NULL);
    for (int i = 0; i < argc_extra; i++) {
        (*env)->SetObjectArrayElement(env, mainArgs, i, (*env)->NewStringUTF(env, argv_extra[i]));
    }

    (*env)->CallStaticVoidMethod(env, cls, mid, mainArgs);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
    }
    (*jvm)->DestroyJavaVM(jvm);
    return 0;
}

static int run_main(JNIEnv *env, JavaVM *jvm, const char *mainclass, int argc_extra, char **argv_extra) {
    char mainclass_slashed[1024];
    snprintf(mainclass_slashed, sizeof(mainclass_slashed), "%s", mainclass);
    dots_to_slashes(mainclass_slashed);

    jclass cls = (*env)->FindClass(env, mainclass_slashed);
    if (cls == NULL) {
        fprintf(stderr, "Could not find class: %s\n", mainclass);
        (*env)->ExceptionDescribe(env);
        (*jvm)->DestroyJavaVM(jvm);
        return 1;
    }

    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "main", "([Ljava/lang/String;)V");
    if (mid == NULL) {
        fprintf(stderr, "Could not find main method\n");
        (*env)->ExceptionDescribe(env);
        (*jvm)->DestroyJavaVM(jvm);
        return 1;
    }

    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    jobjectArray mainArgs = (*env)->NewObjectArray(env, argc_extra > 0 ? argc_extra : 0, stringClass, NULL);
    for (int i = 0; i < argc_extra; i++) {
        (*env)->SetObjectArrayElement(env, mainArgs, i, (*env)->NewStringUTF(env, argv_extra[i]));
    }

    (*env)->CallStaticVoidMethod(env, cls, mid, mainArgs);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
    }
    (*jvm)->DestroyJavaVM(jvm);
    return 0;
}

int main(int argc, char **argv) {
#if __ANDROID_API__ >= 26
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
#endif

    if (argc < 2) {
        fprintf(stderr, "Usage (legacy): %s <jni_libs_dir> <classpath> <MainClass> [args...]\n", argv[0]);
        fprintf(stderr, "Usage (standard): %s [-Xopt ...] [-cp <classpath>] <MainClass> [args...]\n", argv[0]);
        return 1;
    }

    /* javac mode: if invoked as (a symlink named) "javac", skip all of
     * the generic java-style argv parsing below entirely -- real javac
     * command-line syntax (--patch-module=, -d <dir>, source files,
     * etc.) is NOT java's "[opts] MainClass [args]" shape, and trying
     * to parse it that way misreads directory/file arguments as a main
     * class name. Instead, boot the JVM with just enough to dlopen it,
     * then hand ALL of argv[1:] verbatim to KurdApkJavac.main(), which
     * forwards them to javax.tools.JavaCompiler.run() -- the same API
     * real javac uses internally. */
    char argv0_base[256];
    {
        char argv0_copy[1024];
        strncpy(argv0_copy, argv[0], sizeof(argv0_copy) - 1);
        argv0_copy[sizeof(argv0_copy) - 1] = '\0';
        strncpy(argv0_base, basename(argv0_copy), sizeof(argv0_base) - 1);
        argv0_base[sizeof(argv0_base) - 1] = '\0';
    }
    int is_javac_mode = (strcmp(argv0_base, "javac") == 0);

    char libdir[1024];
    const char *java_home_for_prop = NULL;
    const char *classpath = NULL;
    const char *mainclass = NULL;
    char **program_args = NULL;
    int program_argc = 0;

    /* Extra JVM option strings collected from argv when in standard mode. */
    char *extra_opts[64];
    int extra_opts_count = 0;
    /* Buffers to hold combined "flag=value" strings for two-token JVM
     * options (Gradle/Kotlin daemon pass these as two separate argv
     * entries, e.g. "--add-opens" "java.base/sun.nio.ch=ALL-UNNAMED",
     * unlike single-token options like "-Xmx256m"). Without combining
     * them, the value token (which doesn't start with '-') gets
     * misread as the main class. */
    static char two_token_bufs[64][1024];
    int two_token_buf_idx = 0;
    const char *two_token_flags[] = {
        "--add-opens", "--add-exports", "--add-modules",
        "--patch-module", "--add-reads", NULL
    };

    if (is_javac_mode) {
        const char *java_home = getenv("JAVA_HOME");
        if (java_home == NULL) {
            fprintf(stderr, "JAVA_HOME is not set\n");
            return 1;
        }
        snprintf(libdir, sizeof(libdir), "%s/lib", java_home);
        java_home_for_prop = java_home;
        classpath = ".";
        program_args = &argv[1];
        program_argc = argc - 1;
    } else if (argv[1][0] == '-') {
        /* Standard java-style invocation (e.g. from Gradle): auto-detect
         * libdir from our own binary's location, then parse argv[1:]
         * as: [JVM options] [-cp <classpath>] <MainClass> [args...] */
        const char *java_home = getenv("JAVA_HOME");
        if (java_home == NULL) {
            fprintf(stderr, "JAVA_HOME is not set\n");
            return 1;
        }
        snprintf(libdir, sizeof(libdir), "%s/lib", java_home);
        java_home_for_prop = java_home;

        int i = 1;
        while (i < argc) {
            if (strcmp(argv[i], "-cp") == 0 || strcmp(argv[i], "-classpath") == 0) {
                if (i + 1 < argc) {
                    classpath = argv[i + 1];
                    i += 2;
                    continue;
                }
            }
            if (argv[i][0] == '-') {
                int is_two_token = 0;
                for (int f = 0; two_token_flags[f] != NULL; f++) {
                    if (strcmp(argv[i], two_token_flags[f]) == 0) {
                        is_two_token = 1;
                        break;
                    }
                }
                if (is_two_token && i + 1 < argc && two_token_buf_idx < 64
                    && extra_opts_count < 64) {
                    snprintf(two_token_bufs[two_token_buf_idx], sizeof(two_token_bufs[0]),
                             "%s=%s", argv[i], argv[i + 1]);
                    extra_opts[extra_opts_count++] = two_token_bufs[two_token_buf_idx];
                    two_token_buf_idx++;
                    i += 2;
                    continue;
                }
                if (extra_opts_count < 64) {
                    extra_opts[extra_opts_count++] = argv[i];
                }
                i++;
                continue;
            }
            /* First non-option, non-classpath-value token is the main class */
            mainclass = argv[i];
            i++;
            program_args = &argv[i];
            program_argc = argc - i;
            break;
        }

        if (mainclass == NULL) {
            fprintf(stderr, "No main class specified\n");
            return 1;
        }
        if (classpath == NULL) classpath = ".";
    } else {
        /* Legacy invocation: argv[1]=libdir argv[2]=classpath argv[3]=MainClass [args...] */
        if (argc < 4) {
            fprintf(stderr, "Usage: %s <jni_libs_dir> <classpath> <MainClass> [args...]\n", argv[0]);
            return 1;
        }
        snprintf(libdir, sizeof(libdir), "%s", argv[1]);
        classpath = argv[2];
        mainclass = argv[3];
        program_args = &argv[4];
        program_argc = argc - 4;
    }

    /* Capture the caller's original working directory before we chdir
     * into libdir below (needed so dlopen of predeps/libjvm.so below
     * resolves reliably). We chdir back to it right before starting the
     * JVM, because the JVM's "user.dir" system property (and anything
     * that derives a project root from getcwd(), e.g. Gradle's build
     * layout scanner) is initialized from the process's cwd at JVM
     * startup -- leaving cwd at libdir makes Gradle look for a build
     * in $JAVA_HOME/lib instead of the directory the user actually
     * invoked gradlew from. */
    char orig_cwd[1024];
    if (getcwd(orig_cwd, sizeof(orig_cwd)) == NULL) {
        orig_cwd[0] = '\0';
    }

    chdir(libdir);
    /* IMPORTANT: do not export JAVA_HOME=libdir. libdir is already
     * java_home + "/lib" (standard mode) or the raw jni_libs_dir
     * (legacy mode). If we export libdir as JAVA_HOME, any child
     * process that re-derives its own libdir as "$JAVA_HOME/lib"
     * (e.g. Gradle spawning a nested daemon via this same launcher)
     * ends up with a doubled ".../lib/lib" path and fails to find
     * predeps like libandroid-shmem.so. Child processes should see
     * the original, un-suffixed JAVA_HOME so they compute libdir
     * the same way we did. */
    if (java_home_for_prop != NULL) {
        setenv("JAVA_HOME", java_home_for_prop, 1);
    }
    /* Combine the JDK's own lib dir (libdir, needed to dlopen libjvm.so
     * and its predeps below) with whatever LD_LIBRARY_PATH we inherited
     * from our parent process (own_terminal_service.dart's _buildEnv
     * sets this to "$usr/lib:$nativeLibraryDir", which is where bundled
     * tools like git's libpcre2-8.so actually live). Overwriting it with
     * only libdir, as before, broke any child process this JVM spawns
     * (e.g. Gradle -> flutter -> git) that needs those bundled shared
     * libraries: "CANNOT LINK EXECUTABLE git: library libpcre2-8.so not
     * found". */
    const char *inherited_ld_path = getenv("LD_LIBRARY_PATH");
    char combined_ld_path[2048];
    if (inherited_ld_path != NULL && inherited_ld_path[0] != '\0') {
        snprintf(combined_ld_path, sizeof(combined_ld_path), "%s:%s", libdir, inherited_ld_path);
    } else {
        snprintf(combined_ld_path, sizeof(combined_ld_path), "%s", libdir);
    }
    setenv("LD_LIBRARY_PATH", combined_ld_path, 1);

    void *jvm_handle = NULL;
    if (load_predeps_and_jvm(libdir, &jvm_handle) != 0) {
        return 1;
    }

    /* Restore the caller's original directory now that libjvm.so and its
     * predeps are loaded (dlopen already resolved them via absolute
     * paths, so cwd no longer matters for that). This makes the JVM's
     * user.dir property -- and anything that derives a project root
     * from getcwd(), like Gradle's build layout scanner -- match the
     * directory the user actually invoked us from, instead of libdir. */
    if (orig_cwd[0] != '\0') {
        chdir(orig_cwd);
    }

    CreateJavaVM_t JNI_CreateJavaVM_p = (CreateJavaVM_t)dlsym(jvm_handle, "JNI_CreateJavaVM");
    if (!JNI_CreateJavaVM_p) {
        fprintf(stderr, "dlsym JNI_CreateJavaVM failed: %s\n", dlerror());
        return 1;
    }

    char opt_boot[1024], opt_libpath[1024], opt_cp[2048], opt_javahome[1024], opt_tmpdir[1024];
    snprintf(opt_boot, sizeof(opt_boot), "-Dsun.boot.library.path=%s", libdir);
    snprintf(opt_libpath, sizeof(opt_libpath), "-Djava.library.path=%s", libdir);
    snprintf(opt_cp, sizeof(opt_cp), "-Djava.class.path=%s", classpath);
    snprintf(opt_javahome, sizeof(opt_javahome), "-Djava.home=%s",
             java_home_for_prop ? java_home_for_prop : libdir);
    /* This JDK build is originally a Termux package and some of its
     * internal defaults (e.g. java.io.tmpdir fallback) resolve to
     * Termux's own fixed prefix path rather than honoring our TMPDIR
     * env var, which breaks Gradle's VirtualFileSystem init with
     * "java.io.tmpdir is set to a directory that doesn't exist".
     * Force it explicitly from our own TMPDIR (set in _buildEnv as
     * usr/tmp, which bootstrap guarantees exists on every launch). */
    const char *tmpdir_env = getenv("TMPDIR");
    int have_tmpdir_opt = 0;
    if (tmpdir_env != NULL && tmpdir_env[0] != '\0') {
        snprintf(opt_tmpdir, sizeof(opt_tmpdir), "-Djava.io.tmpdir=%s", tmpdir_env);
        have_tmpdir_opt = 1;
    }

    /* base options + one for --add-exports + any extra_opts collected */
    JavaVMOption options[7 + 64];
    int nopts = 0;
    options[nopts++].optionString = opt_boot;
    options[nopts++].optionString = opt_libpath;
    options[nopts++].optionString = opt_cp;
    options[nopts++].optionString = opt_javahome;
    if (have_tmpdir_opt) {
        options[nopts++].optionString = opt_tmpdir;
    }
    /* jspawnhelper (the native helper OpenJDK's ProcessBuilder normally
     * execs to fork+exec a child process) is itself an ELF binary under
     * app_flutter and therefore blocked by Android's noexec mount, same
     * as any other file there. VFORK mode makes ProcessBuilder do the
     * fork+exec inline instead of shelling out to jspawnhelper, which is
     * required for anything this JVM spawns to work at all -- e.g.
     * Gradle forking its own build daemon as a nested java process. */
    options[nopts++].optionString = "-Djdk.lang.Process.launchMechanism=VFORK";
    options[nopts++].optionString = "--add-exports=jdk.compiler/com.sun.tools.javac.main=ALL-UNNAMED";
    if (is_javac_mode) {
        /* Force the compiler modules into the default root module set --
         * they can be present in the jlink image (observable) without
         * being auto-resolved for classpath/unnamed-module execution,
         * which is what KurdApkJavac.main() runs as when invoked via
         * this launcher. */
        options[nopts++].optionString = "--add-modules=java.compiler,jdk.compiler";
    }
    for (int i = 0; i < extra_opts_count; i++) {
        options[nopts++].optionString = extra_opts[i];
    }

    JavaVMInitArgs vm_args;
    vm_args.version = JNI_VERSION_1_6;
    vm_args.nOptions = nopts;
    vm_args.options = options;
    vm_args.ignoreUnrecognized = JNI_TRUE;

    JavaVM *jvm;
    JNIEnv *env;
    jint res = JNI_CreateJavaVM_p(&jvm, (void**)&env, &vm_args);
    if (res != JNI_OK) {
        fprintf(stderr, "JNI_CreateJavaVM failed: %d\n", res);
        return 1;
    }

    printf("JVM created successfully!\n");
    if (is_javac_mode) {
        return run_kurdapk_javac(env, jvm, program_argc, program_args);
    }
    return run_main(env, jvm, mainclass, program_argc, program_args);
}

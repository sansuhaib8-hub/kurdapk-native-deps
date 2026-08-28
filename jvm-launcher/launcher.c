#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <malloc.h>
#include <libgen.h>

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

    char libdir[1024];
    const char *classpath = NULL;
    const char *mainclass = NULL;
    char **program_args = NULL;
    int program_argc = 0;

    /* Extra JVM option strings collected from argv when in standard mode. */
    char *extra_opts[16];
    int extra_opts_count = 0;

    if (argv[1][0] == '-') {
        /* Standard java-style invocation (e.g. from Gradle): auto-detect
         * libdir from our own binary's location, then parse argv[1:]
         * as: [JVM options] [-cp <classpath>] <MainClass> [args...] */
        const char *java_home = getenv("JAVA_HOME");
        if (java_home == NULL) {
            fprintf(stderr, "JAVA_HOME is not set\n");
            return 1;
        }
        snprintf(libdir, sizeof(libdir), "%s/lib", java_home);

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
                if (extra_opts_count < 16) {
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

    chdir(libdir);
    setenv("JAVA_HOME", libdir, 1);
    setenv("LD_LIBRARY_PATH", libdir, 1);

    void *jvm_handle = NULL;
    if (load_predeps_and_jvm(libdir, &jvm_handle) != 0) {
        return 1;
    }

    CreateJavaVM_t JNI_CreateJavaVM_p = (CreateJavaVM_t)dlsym(jvm_handle, "JNI_CreateJavaVM");
    if (!JNI_CreateJavaVM_p) {
        fprintf(stderr, "dlsym JNI_CreateJavaVM failed: %s\n", dlerror());
        return 1;
    }

    char opt_boot[1024], opt_libpath[1024], opt_cp[2048];
    snprintf(opt_boot, sizeof(opt_boot), "-Dsun.boot.library.path=%s", libdir);
    snprintf(opt_libpath, sizeof(opt_libpath), "-Djava.library.path=%s", libdir);
    snprintf(opt_cp, sizeof(opt_cp), "-Djava.class.path=%s", classpath);

    /* base options + one for --add-exports + any extra_opts collected */
    JavaVMOption options[4 + 16];
    int nopts = 0;
    options[nopts++].optionString = opt_boot;
    options[nopts++].optionString = opt_libpath;
    options[nopts++].optionString = opt_cp;
    options[nopts++].optionString = "--add-exports=jdk.compiler/com.sun.tools.javac.main=ALL-UNNAMED";
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
    return run_main(env, jvm, mainclass, program_argc, program_args);
}

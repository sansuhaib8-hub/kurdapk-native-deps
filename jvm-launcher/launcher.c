#include <jni.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <malloc.h>

typedef jint (JNICALL *CreateJavaVM_t)(JavaVM **, void **, void *);

int main(int argc, char **argv) {
#if __ANDROID_API__ >= 26
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
#endif
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <jni_libs_dir> <classpath> <MainClass> [args...]\n", argv[0]);
        return 1;
    }

    const char *libdir = argv[1];
    chdir(libdir);
    setenv("JAVA_HOME", libdir, 1);
    setenv("LD_LIBRARY_PATH", libdir, 1);
    const char *classpath = argv[2];
    const char *mainclass = argv[3];

    char path_buf[1024];

    /* پێویستە پێش libjvm.so بە دەستی dlopen بکرێن */
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

    CreateJavaVM_t JNI_CreateJavaVM_p = (CreateJavaVM_t)dlsym(jvm_handle, "JNI_CreateJavaVM");
    if (!JNI_CreateJavaVM_p) {
        fprintf(stderr, "dlsym JNI_CreateJavaVM failed: %s\n", dlerror());
        return 1;
    }

    char opt_boot[1024], opt_libpath[1024], opt_cp[2048];
    snprintf(opt_boot, sizeof(opt_boot), "-Dsun.boot.library.path=%s", libdir);
    snprintf(opt_libpath, sizeof(opt_libpath), "-Djava.library.path=%s", libdir);
    snprintf(opt_cp, sizeof(opt_cp), "-Djava.class.path=%s", classpath);

    JavaVMOption options[3];
    options[0].optionString = opt_boot;
    options[1].optionString = opt_libpath;
    options[2].optionString = opt_cp;

    JavaVMInitArgs vm_args;
    vm_args.version = JNI_VERSION_1_6;
    vm_args.nOptions = 3;
    vm_args.options = options;
    vm_args.ignoreUnrecognized = JNI_FALSE;

    JavaVM *jvm;
    JNIEnv *env;
    jint res = JNI_CreateJavaVM_p(&jvm, (void**)&env, &vm_args);
    if (res != JNI_OK) {
        fprintf(stderr, "JNI_CreateJavaVM failed: %d\n", res);
        return 1;
    }

    printf("JVM created successfully!\n");

    jclass cls = (*env)->FindClass(env, mainclass);
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
    jobjectArray mainArgs = (*env)->NewObjectArray(env, argc - 4 > 0 ? argc - 4 : 0, stringClass, NULL);
    for (int i = 4; i < argc; i++) {
        (*env)->SetObjectArrayElement(env, mainArgs, i - 4, (*env)->NewStringUTF(env, argv[i]));
    }

    (*env)->CallStaticVoidMethod(env, cls, mid, mainArgs);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
    }

    (*jvm)->DestroyJavaVM(jvm);
    return 0;
}

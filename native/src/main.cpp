// Granny multiplayer modu - native kisim (v0.2: konum okuma)
// RVA degerleri Granny 1.8.12 (arm64) dump.cs dosyasindandir.
#include <jni.h>
#include <android/log.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>

#define TAG "GrannyMP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static JavaVM *g_vm = nullptr;
static jclass g_modClass = nullptr;
static jmethodID g_onPos = nullptr;
static jmethodID g_onLog = nullptr;
static uintptr_t g_base = 0;

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

typedef void (*GetVec3Fn)(intptr_t self, Vec3 *ret, const void *mi);
typedef void (*GetQuatFn)(intptr_t self, Quat *ret, const void *mi);
typedef void (*FixedUpdateFn)(void *self, const void *mi);

// FPSControllerNEW.FixedUpdate
static const uintptr_t RVA_FIXED_UPDATE = 0x14E4268;
// Transform.get_position_Injected / get_rotation_Injected
static const uintptr_t RVA_GET_POS = 0x2AA9120;
static const uintptr_t RVA_GET_ROT = 0x2AA9848;
// FPSControllerNEW.thisTransform alani
static const uintptr_t OFF_THIS_TRANSFORM = 0x68;
// UnityEngine.Object.m_CachedPtr
static const uintptr_t OFF_CACHED_PTR = 0x10;

// ---- Basit arm64 inline hook (Dobby gerektirmez) ----
// Hedef fonksiyonun ilk 16 baytini (4 komut) kopyalar. Bu komutlar PC-bagimli olmamalidir.
// Donus: 0 = tamam, 1 = imza uyusmuyor (farkli oyun surumu), 2 = mmap, 3 = mprotect
static int inline_hook(void *target, void *hook, void **orig, const uint32_t expect[4]) {
    uint32_t *t = (uint32_t *) target;
    for (int i = 0; i < 4; i++) {
        if (t[i] != expect[i]) return 1;
    }
    void *mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return 2;
    uint32_t *tr = (uint32_t *) mem;
    memcpy(tr, t, 16);
    tr[4] = 0x58000050; // ldr x16, #8
    tr[5] = 0xd61f0200; // br  x16
    *(uint64_t *) (tr + 6) = (uint64_t) (t + 4);
    __builtin___clear_cache((char *) mem, (char *) mem + 32);
    if (mprotect(mem, 4096, PROT_READ | PROT_EXEC) != 0) return 3;

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t) t & ~((uintptr_t) ps - 1);
    size_t len = ((uintptr_t) t + 16 > page + ps) ? (size_t) (2 * ps) : (size_t) ps;
    if (mprotect((void *) page, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 3;
    uint32_t patch[4];
    patch[0] = 0x58000050; // ldr x16, #8
    patch[1] = 0xd61f0200; // br  x16
    *(uint64_t *) (patch + 2) = (uint64_t) hook;
    *orig = mem;
    memcpy(t, patch, 16);
    mprotect((void *) page, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *) t, (char *) t + 16);
    return 0;
}

static JNIEnv *env_for_thread() {
    static thread_local JNIEnv *env = nullptr;
    if (env) return env;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv((void **) &env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
    }
    return env;
}

static void jlog(const char *msg) {
    LOGI("%s", msg);
    JNIEnv *env = env_for_thread();
    if (!env || !g_modClass || !g_onLog) return;
    jstring s = env->NewStringUTF(msg);
    env->CallStaticVoidMethod(g_modClass, g_onLog, s);
    env->DeleteLocalRef(s);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static FixedUpdateFn orig_FixedUpdate = nullptr;
static int g_tick = 0;
static bool g_firstPos = false;

static void hook_FixedUpdate(void *self, const void *mi) {
    orig_FixedUpdate(self, mi);
    if (!self || !g_base) return;
    if (++g_tick % 5 != 0) return; // 50 Hz -> 10 Hz

    void *tr = *(void **) ((uintptr_t) self + OFF_THIS_TRANSFORM);
    if (!tr) return;
    intptr_t cached = *(intptr_t *) ((uintptr_t) tr + OFF_CACHED_PTR);
    if (!cached) return;

    Vec3 p = {0, 0, 0};
    Quat q = {0, 0, 0, 1};
    ((GetVec3Fn) (g_base + RVA_GET_POS))(cached, &p, nullptr);
    ((GetQuatFn) (g_base + RVA_GET_ROT))(cached, &q, nullptr);
    float yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * 57.29578f;

    if (!g_firstPos) {
        g_firstPos = true;
        char buf[96];
        snprintf(buf, sizeof(buf), "Ilk konum okundu: %.1f %.1f %.1f", p.x, p.y, p.z);
        jlog(buf);
    }

    JNIEnv *env = env_for_thread();
    if (!env || !g_modClass || !g_onPos) return;
    env->CallStaticVoidMethod(g_modClass, g_onPos, p.x, p.y, p.z, yaw);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static int phdr_cb(struct dl_phdr_info *info, size_t, void *data) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
        *(uintptr_t *) data = (uintptr_t) info->dlpi_addr;
        return 1;
    }
    return 0;
}

static void *init_thread(void *) {
    uintptr_t base = 0;
    for (int i = 0; i < 1500 && !base; i++) {
        dl_iterate_phdr(phdr_cb, &base);
        if (!base) usleep(200000);
    }
    if (!base) {
        jlog("libil2cpp bulunamadi");
        return nullptr;
    }
    g_base = base;
    jlog("libil2cpp bulundu");
    // FPSControllerNEW.FixedUpdate ilk 4 komut (1.8.12 arm64)
    static const uint32_t expect[4] = {0xd10383ff, 0xfd002bee, 0x6d0633ed, 0x6d072beb};
    void *orig = nullptr;
    int r = inline_hook((void *) (base + RVA_FIXED_UPDATE), (void *) hook_FixedUpdate, &orig, expect);
    if (r == 0) {
        orig_FixedUpdate = (FixedUpdateFn) orig;
        jlog("Hook kuruldu (FPSControllerNEW)");
    } else if (r == 1) {
        jlog("Hook basarisiz: oyun surumu farkli (imza uyusmuyor)");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Hook basarisiz: kod %d, errno %d", r, errno);
        jlog(buf);
    }
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    g_vm = vm;
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass c = env->FindClass("com/dvloper/mod/Mod");
    if (!c) {
        env->ExceptionClear();
        return JNI_VERSION_1_6;
    }
    g_modClass = (jclass) env->NewGlobalRef(c);
    g_onPos = env->GetStaticMethodID(c, "onLocalPos", "(FFFF)V");
    g_onLog = env->GetStaticMethodID(c, "nativeLog", "(Ljava/lang/String;)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    pthread_t t;
    pthread_create(&t, nullptr, init_thread, nullptr);
    pthread_detach(t);
    return JNI_VERSION_1_6;
}

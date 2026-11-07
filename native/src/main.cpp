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
#include <dlfcn.h>

#define TAG "GrannyMP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static JavaVM *g_vm = nullptr;
static jclass g_modClass = nullptr;
static jmethodID g_onPos = nullptr;
static jmethodID g_onLog = nullptr;
static jmethodID g_onTag = nullptr;
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
// Hedef fonksiyonun ilk 16 baytini (4 komut) trambolene kopyalar. ADRP komutlari
// mutlak adrese cevrilir; baska PC-bagimli komut varsa desteklenmez.
// Donus: 0 = tamam, 1 = imza uyusmuyor (farkli oyun surumu), 2 = mmap, 3 = mprotect,
//        4 = desteklenmeyen PC-bagimli komut
static int inline_hook(void *target, void *hook, void **orig, const uint32_t expect[4]) {
    uint32_t *t = (uint32_t *) target;
    for (int i = 0; i < 4; i++) {
        if (t[i] != expect[i]) return 1;
    }
    void *mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return 2;
    uint32_t *tr = (uint32_t *) mem;
    uint64_t lits[4];
    int nl = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t w = t[i];
        if ((w & 0x9F000000) == 0x90000000) { // ADRP
            uint32_t rd = w & 0x1F;
            int64_t immhi = (w >> 5) & 0x7FFFF;
            int64_t immlo = (w >> 29) & 3;
            int64_t imm = (immhi << 2) | immlo;
            if (imm & 0x100000) imm -= 0x200000;
            uint64_t pc = (uint64_t) (t + i);
            uint64_t page = (pc & ~0xFFFULL) + (uint64_t) (imm * 4096);
            int litOff = 32 + 8 * nl - 4 * i; // bayt
            tr[i] = 0x58000000 | ((uint32_t) (litOff / 4) << 5) | rd; // ldr xd, literal
            lits[nl++] = page;
        } else if ((w & 0x9F000000) == 0x10000000 || (w & 0x3B000000) == 0x18000000 ||
                   (w & 0x7C000000) == 0x14000000 || (w & 0x7E000000) == 0x34000000 ||
                   (w & 0x7E000000) == 0x36000000 || (w & 0xFF000010) == 0x54000000) {
            munmap(mem, 4096);
            return 4;
        } else {
            tr[i] = w;
        }
    }
    tr[4] = 0x58000050; // ldr x16, #8
    tr[5] = 0xd61f0200; // br  x16
    *(uint64_t *) (tr + 6) = (uint64_t) (t + 4);
    for (int j = 0; j < nl; j++) *(uint64_t *) (tr + 8 + 2 * j) = lits[j];
    __builtin___clear_cache((char *) mem, (char *) mem + 64);
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

// --- Uzak oyuncular (Java'dan gelir, oyun ana is parcaciginda okunur) ---
struct Remote {
    int id;
    float x, y, z;
    bool used;
};
static Remote g_remote[8];
static pthread_mutex_t g_rm = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_remoteCount = 0;

typedef void *(*GetCamFn)(const void *mi);
typedef Vec3 (*W2SFn)(void *cam, Vec3 pos, const void *mi);
typedef int (*GetIntFn)(const void *mi);
static const uintptr_t RVA_CAM_MAIN = 0x2A6DBF8;     // Camera.get_main
static const uintptr_t RVA_W2S = 0x2A6D978;          // Camera.WorldToScreenPoint(Vector3)
static const uintptr_t RVA_SCREEN_W = 0x2A7695C;     // Screen.get_width
static const uintptr_t RVA_SCREEN_H = 0x2A76984;     // Screen.get_height

// Kamera nesnesine ulasilamadigi icin (Camera.main bos) projeksiyonu kendimiz hesapliyoruz.
// Kamera = FPSControllerNEW.cameraPivot. Dikey gorus acisi tahminidir (derece).
static const uintptr_t OFF_CAMERA_PIVOT = 0x38;
static float g_fovDeg = 60.0f;

static Vec3 rot_inv(const Quat &q, const Vec3 &v) {
    float qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w; // eslenik
    float tx = 2.0f * (qy * v.z - qz * v.y);
    float ty = 2.0f * (qz * v.x - qx * v.z);
    float tz = 2.0f * (qx * v.y - qy * v.x);
    Vec3 r;
    r.x = v.x + qw * tx + (qy * tz - qz * ty);
    r.y = v.y + qw * ty + (qz * tx - qx * tz);
    r.z = v.z + qw * tz + (qx * ty - qy * tx);
    return r;
}

static void update_tags(const Vec3 &me, intptr_t pivotPtr) {
    Remote snap[8];
    int n = 0;
    pthread_mutex_lock(&g_rm);
    for (int i = 0; i < 8; i++) if (g_remote[i].used) snap[n++] = g_remote[i];
    pthread_mutex_unlock(&g_rm);
    if (n == 0) return;
    JNIEnv *env = env_for_thread();
    if (!env || !g_modClass || !g_onTag) return;

    int sw = ((GetIntFn) (g_base + RVA_SCREEN_W))(nullptr);
    int sh = ((GetIntFn) (g_base + RVA_SCREEN_H))(nullptr);

    Vec3 cp = me;
    Quat cq = {0, 0, 0, 1};
    bool haveCam = false;
    if (pivotPtr) {
        ((GetVec3Fn) (g_base + RVA_GET_POS))(pivotPtr, &cp, nullptr);
        ((GetQuatFn) (g_base + RVA_GET_ROT))(pivotPtr, &cq, nullptr);
        haveCam = true;
    }
    float eyeH = cp.y - me.y;
    float tanHalf = tanf(g_fovDeg * 0.5f * 0.01745329f);
    float aspect = (sh > 0) ? (float) sw / (float) sh : 1.7778f;

    for (int i = 0; i < n; i++) {
        float sx = 0, sy = 0, sz = -1;
        Vec3 d = {snap[i].x - cp.x, snap[i].y + eyeH + 0.5f - cp.y, snap[i].z - cp.z};
        if (haveCam && sh > 0) {
            Vec3 c = rot_inv(cq, d);
            if (c.z > 0.1f) {
                sz = c.z;
                sx = (c.x / (c.z * tanHalf * aspect) * 0.5f + 0.5f) * (float) sw;
                sy = (c.y / (c.z * tanHalf) * 0.5f + 0.5f) * (float) sh;
            }
        }
        float dx = snap[i].x - me.x, dy = snap[i].y - me.y, dz = snap[i].z - me.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        static int dbgCall = 0, dbgLogs = 0;
        if (i == 0 && dbgLogs < 0 && (dbgCall++ % 75) == 0) {
            dbgLogs++;
            char b[160];
            snprintf(b, sizeof(b), "etiket: kamera=%s sx=%.0f sy=%.0f sz=%.1f mesafe=%.1f",
                     haveCam ? "var" : "YOK", sx, sy, sz, dist);
            jlog(b);
        }
        env->CallStaticVoidMethod(g_modClass, g_onTag, (jint) snap[i].id, sx, sy, sz,
                                  (jfloat) sw, (jfloat) sh, dist);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

static void hook_FixedUpdate(void *self, const void *mi) {
    orig_FixedUpdate(self, mi);
    if (!self || !g_base) return;
    g_tick++;
    bool doSend = (g_tick % 5 == 0);          // 50 Hz -> 10 Hz
    bool doTags = (g_tick % 2 == 0) && g_remoteCount > 0; // 25 Hz
    if (!doSend && !doTags) return;

    void *tr = *(void **) ((uintptr_t) self + OFF_THIS_TRANSFORM);
    if (!tr) return;
    intptr_t cached = *(intptr_t *) ((uintptr_t) tr + OFF_CACHED_PTR);
    if (!cached) return;

    Vec3 p = {0, 0, 0};
    Quat q = {0, 0, 0, 1};
    ((GetVec3Fn) (g_base + RVA_GET_POS))(cached, &p, nullptr);
    ((GetQuatFn) (g_base + RVA_GET_ROT))(cached, &q, nullptr);

    if (doSend) {
        float yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * 57.29578f;
        if (!g_firstPos) {
            g_firstPos = true;
            char buf[96];
            snprintf(buf, sizeof(buf), "Ilk konum okundu: %.1f %.1f %.1f", p.x, p.y, p.z);
            jlog(buf);
        }
        JNIEnv *env = env_for_thread();
        if (env && g_modClass && g_onPos) {
            env->CallStaticVoidMethod(g_modClass, g_onPos, p.x, p.y, p.z, yaw);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    if (doTags) {
        void *pv = *(void **) ((uintptr_t) self + OFF_CAMERA_PIVOT);
        intptr_t pc = pv ? *(intptr_t *) ((uintptr_t) pv + OFF_CACHED_PTR) : 0;
        update_tags(p, pc);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeSetRemote(JNIEnv *, jclass, jint id, jfloat x, jfloat y, jfloat z) {
    pthread_mutex_lock(&g_rm);
    int slot = -1;
    for (int i = 0; i < 8; i++) if (g_remote[i].used && g_remote[i].id == id) slot = i;
    if (slot < 0) for (int i = 0; i < 8; i++) if (!g_remote[i].used) { slot = i; break; }
    if (slot >= 0) {
        g_remote[slot].used = true;
        g_remote[slot].id = id;
        g_remote[slot].x = x;
        g_remote[slot].y = y;
        g_remote[slot].z = z;
    }
    int c = 0;
    for (int i = 0; i < 8; i++) if (g_remote[i].used) c++;
    g_remoteCount = c;
    pthread_mutex_unlock(&g_rm);
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeRemoveRemote(JNIEnv *, jclass, jint id) {
    pthread_mutex_lock(&g_rm);
    int c = 0;
    for (int i = 0; i < 8; i++) {
        if (g_remote[i].used && g_remote[i].id == id) g_remote[i].used = false;
        if (g_remote[i].used) c++;
    }
    g_remoteCount = c;
    pthread_mutex_unlock(&g_rm);
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeClearRemote(JNIEnv *, jclass) {
    pthread_mutex_lock(&g_rm);
    for (int i = 0; i < 8; i++) g_remote[i].used = false;
    g_remoteCount = 0;
    pthread_mutex_unlock(&g_rm);
}

// --- Ana is parcacigi pompasi: EventSystem.Update (menu ve oyunda calisir) ---
typedef void (*UpdateFn)(void *self, const void *mi);
typedef void *(*StringNewFn)(const char *);
typedef void (*LoadSceneFn)(void *name, const void *mi);
static UpdateFn orig_EsUpdate = nullptr;
static volatile int g_startReq = 0;
static volatile int g_startSeed = 0;
static volatile int g_gameSeed = 0;
typedef void (*InitStateFn)(int seed, const void *mi);
static const uintptr_t RVA_RANDOM_INIT = 0x2A953BC; // UnityEngine.Random.InitState(int)
static const uintptr_t RVA_ES_UPDATE = 0x2D5E590;   // EventSystem.Update
static const uintptr_t RVA_LOAD_SCENE = 0x2AAF058;  // SceneManager.LoadScene(string)

static void hook_EsUpdate(void *self, const void *mi) {
    orig_EsUpdate(self, mi);
    if (!g_startReq) return;
    g_startReq = 0;
    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    StringNewFn sn = h ? (StringNewFn) dlsym(h, "il2cpp_string_new") : nullptr;
    if (!sn) {
        jlog("sahne: il2cpp_string_new bulunamadi");
        return;
    }
    int seed = g_startSeed;
    if (seed != 0) {
        ((InitStateFn) (g_base + RVA_RANDOM_INIT))(seed, nullptr);
        char b[64];
        snprintf(b, sizeof(b), "Rastgele tohum ayarlandi: %d", seed);
        jlog(b);
    }
    jlog("Sahne yukleniyor: Scene");
    void *nm = sn("Scene");
    ((LoadSceneFn) (g_base + RVA_LOAD_SCENE))(nm, nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeStartGame(JNIEnv *, jclass, jint seed) {
    g_startSeed = seed;
    g_gameSeed = seed;
    g_startReq = 1;
}

// --- Rastgele yerlesim: Start'tan hemen once tohumu yeniden ayarla ---
static UpdateFn orig_KeyStart = nullptr;
static UpdateFn orig_DunkStart = nullptr;
static UpdateFn orig_KofotStart = nullptr;
static const uintptr_t RVA_KEY_START = 0x1557328;    // RandomKeyPlace.Start
static const uintptr_t RVA_DUNK_START = 0x1557298;   // RandomDunkPlace.Start
static const uintptr_t RVA_KOFOT_START = 0x155739C;  // RandomKofotPlace.Start

static void seed_before(int salt) {
    int s = g_gameSeed;
    if (s != 0) ((InitStateFn) (g_base + RVA_RANDOM_INIT))(s * 31 + salt, nullptr);
}

static void log_place(const char *what, int n) {
    char b[80];
    snprintf(b, sizeof(b), "yer: %s=%d (tohum %d)", what, n, (int) g_gameSeed);
    jlog(b);
}

static void hook_KeyStart(void *self, const void *mi) {
    seed_before(1);
    orig_KeyStart(self, mi);
    log_place("anahtar", *(int *) ((uintptr_t) self + 0x38));
}

static void hook_DunkStart(void *self, const void *mi) {
    seed_before(2);
    orig_DunkStart(self, mi);
    log_place("dunk", *(int *) ((uintptr_t) self + 0x40));
}

static void hook_KofotStart(void *self, const void *mi) {
    seed_before(3);
    orig_KofotStart(self, mi);
    log_place("kofot", *(int *) ((uintptr_t) self + 0x38));
}

// --- Gun baslangici: StartNewDay (esya yerlesimi) rastgele sayilari tohumlanir ---
typedef bool (*MoveNextFn)(void *self, const void *mi);
static MoveNextFn orig_DayStart = nullptr;
static MoveNextFn orig_DayAds = nullptr;
static const uintptr_t RVA_DAY_START = 0x154F254;  // StartNewDay.<Start>d__132.MoveNext
static const uintptr_t RVA_DAY_ADS = 0x1550B38;    // StartNewDay.<newDayAfterADS>d__142.MoveNext
static int g_dayLogs = 0;

static void day_seed(void *self, int salt, const char *name) {
    int st = *(int *) ((uintptr_t) self + 0x10); // <>1__state
    int seed = g_gameSeed;
    if (seed == 0) return;
    ((InitStateFn) (g_base + RVA_RANDOM_INIT))(seed * 131 + st + salt, nullptr);
    if (g_dayLogs < 6) {
        g_dayLogs++;
        char b[96];
        snprintf(b, sizeof(b), "gun tohumu: %s durum=%d (tohum %d)", name, st, seed);
        jlog(b);
    }
}

static bool hook_DayStart(void *self, const void *mi) {
    day_seed(self, 7, "Start");
    return orig_DayStart(self, mi);
}

static bool hook_DayAds(void *self, const void *mi) {
    day_seed(self, 11, "YeniGun");
    return orig_DayAds(self, mi);
}

// --- PlayerPrefs.GetInt(string,int) gozlemi: hangi kayitli ayarlar okunuyor? ---
typedef int (*PrefGetIntFn)(void *key, int def, const void *mi);
static PrefGetIntFn orig_PrefGetInt = nullptr;
static const uintptr_t RVA_PREF_GETINT = 0x2A945E8;

struct PrefEnt {
    char key[40];
    int val;
    bool used;
};
static PrefEnt g_prefs[64];

static void il2str_ascii(void *s, char *out, int cap) {
    if (!s) {
        out[0] = 0;
        return;
    }
    int len = *(int *) ((uintptr_t) s + 0x10);
    const uint16_t *ch = (const uint16_t *) ((uintptr_t) s + 0x14);
    int n = len < cap - 1 ? len : cap - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) out[i] = ch[i] < 128 ? (char) ch[i] : '?';
    out[n] = 0;
}

static int hook_PrefGetInt(void *key, int def, const void *mi) {
    int r = orig_PrefGetInt(key, def, mi);
    char k[40];
    il2str_ascii(key, k, sizeof(k));
    int slot = -1;
    for (int i = 0; i < 64; i++) {
        if (g_prefs[i].used && strcmp(g_prefs[i].key, k) == 0) {
            slot = i;
            break;
        }
    }
    char b[96];
    if (slot < 0) {
        for (int i = 0; i < 64; i++) {
            if (!g_prefs[i].used) {
                g_prefs[i].used = true;
                strncpy(g_prefs[i].key, k, sizeof(g_prefs[i].key) - 1);
                g_prefs[i].val = r;
                snprintf(b, sizeof(b), "kayit: %s=%d", k, r);
                jlog(b);
                break;
            }
        }
    } else if (g_prefs[slot].val != r) {
        g_prefs[slot].val = r;
        snprintf(b, sizeof(b), "kayit: %s=%d (degisti)", k, r);
        jlog(b);
    }
    return r;
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
        static const uint32_t expect2[4] = {0xa9bc67fe, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
        void *orig2 = nullptr;
        int r2 = inline_hook((void *) (base + RVA_ES_UPDATE), (void *) hook_EsUpdate, &orig2, expect2);
        if (r2 == 0) {
            orig_EsUpdate = (UpdateFn) orig2;
            jlog("Hook kuruldu (EventSystem)");
        } else {
            jlog(r2 == 1 ? "EventSystem hook: imza uyusmuyor" : "EventSystem hook basarisiz");
        }
        static const uint32_t expKey[4] = {0xa9bf4ffe, 0xaa0003f3, 0x52800020, 0x52800081};
        static const uint32_t expDunk[4] = {0xa9bf4ffe, 0xaa0003f3, 0x52800020, 0x528000a1};
        void *o3 = nullptr, *o4 = nullptr, *o5 = nullptr;
        int r3 = inline_hook((void *) (base + RVA_KEY_START), (void *) hook_KeyStart, &o3, expKey);
        int r4 = inline_hook((void *) (base + RVA_DUNK_START), (void *) hook_DunkStart, &o4, expDunk);
        int r5 = inline_hook((void *) (base + RVA_KOFOT_START), (void *) hook_KofotStart, &o5, expKey);
        if (r3 == 0) orig_KeyStart = (UpdateFn) o3;
        if (r4 == 0) orig_DunkStart = (UpdateFn) o4;
        if (r5 == 0) orig_KofotStart = (UpdateFn) o5;
        char hb[80];
        snprintf(hb, sizeof(hb), "Yerlesim hook: anahtar=%d dunk=%d kofot=%d (0=tamam)", r3, r4, r5);
        jlog(hb);
        static const uint32_t expDay1[4] = {0xa9bd5ffe, 0xa90157f6, 0xa9024ff4, 0xb000dc54};
        static const uint32_t expDay2[4] = {0xfc1a0fea, 0x6d0123e9, 0xa90267fe, 0xa9035ff8};
        void *o6 = nullptr, *o7 = nullptr;
        int r6 = inline_hook((void *) (base + RVA_DAY_START), (void *) hook_DayStart, &o6, expDay1);
        int r7 = inline_hook((void *) (base + RVA_DAY_ADS), (void *) hook_DayAds, &o7, expDay2);
        if (r6 == 0) orig_DayStart = (MoveNextFn) o6;
        if (r7 == 0) orig_DayAds = (MoveNextFn) o7;
        char db[80];
        snprintf(db, sizeof(db), "Gun hook: start=%d yenigun=%d (0=tamam)", r6, r7);
        jlog(db);
        static const uint32_t expPref[4] = {0xd10143ff, 0xa90357fe, 0xa9044ff4, 0xf0003255};
        void *o8 = nullptr;
        int r8 = inline_hook((void *) (base + RVA_PREF_GETINT), (void *) hook_PrefGetInt, &o8, expPref);
        if (r8 == 0) orig_PrefGetInt = (PrefGetIntFn) o8;
        char pb[64];
        snprintf(pb, sizeof(pb), "Kayit hook: %d (0=tamam)", r8);
        jlog(pb);
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
    g_onTag = env->GetStaticMethodID(c, "onRemoteScreen", "(IFFFFFF)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    pthread_t t;
    pthread_create(&t, nullptr, init_thread, nullptr);
    pthread_detach(t);
    return JNI_VERSION_1_6;
}

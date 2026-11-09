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
#include <time.h>

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
typedef int (*PrefGetIntFn)(void *key, int def, const void *mi);
typedef void (*PrefSetIntFn)(void *key, int val, const void *mi);
static PrefGetIntFn orig_PrefGetInt = nullptr;
static const uintptr_t RVA_PREF_GETINT = 0x2A945E8;  // PlayerPrefs.GetInt(string,int)
static const uintptr_t RVA_PREF_SETINT = 0x2A94590;  // PlayerPrefs.SetInt(string,int)
static volatile int g_startAnnounce = 0;
static volatile int g_hostNR = -1;
static jmethodID g_onHostStart = nullptr;
static jmethodID g_onGranny = nullptr;
static volatile int g_gHave = 0;
static volatile int g_blockGranny = 0;
static const char *g_ctx = nullptr;       // olum/yeni gun sirasinda Granny'yi isinlama
static intptr_t g_grannyCached = 0;          // Granny transform (yerel isaretci)
static void *g_grannySelf = nullptr;         // EnemyAIGranny ornegi
static jmethodID g_onGrannyAnim = nullptr;
static jmethodID g_onWorldAnim = nullptr;
static volatile int g_animReset = 0;
static void process_world_anims();
static volatile int g_startSeed = 0;
static volatile int g_gameSeed = 0;
typedef void (*InitStateFn)(int seed, const void *mi);
static const uintptr_t RVA_RANDOM_INIT = 0x2A953BC; // UnityEngine.Random.InitState(int)
static const uintptr_t RVA_ES_UPDATE = 0x2D5E590;   // EventSystem.Update
static const uintptr_t RVA_LOAD_SCENE = 0x2AAF058;  // SceneManager.LoadScene(string)

static void hook_EsUpdate(void *self, const void *mi) {
    orig_EsUpdate(self, mi);
    process_world_anims();
    if (!g_startReq) return;
    g_startReq = 0;
    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    StringNewFn sn = h ? (StringNewFn) dlsym(h, "il2cpp_string_new") : nullptr;
    if (!sn) {
        jlog("sahne: il2cpp_string_new bulunamadi");
        return;
    }
    {
        void *key = sn("randomNR");
        if (g_startAnnounce) {
            g_startAnnounce = 0;
            PrefGetIntFn getter = orig_PrefGetInt ? orig_PrefGetInt : (PrefGetIntFn) (g_base + RVA_PREF_GETINT);
            int nr = getter(key, 0, nullptr);
            JNIEnv *env = env_for_thread();
            if (env && g_modClass && g_onHostStart) {
                env->CallStaticVoidMethod(g_modClass, g_onHostStart, (jint) nr);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        } else if (g_hostNR >= 0) {
            ((PrefSetIntFn) (g_base + RVA_PREF_SETINT))(key, g_hostNR, nullptr);
            char b[64];
            snprintf(b, sizeof(b), "kayit ayarlandi: randomNR=%d", (int) g_hostNR);
            jlog(b);
        }
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
Java_com_dvloper_mod_Mod_nativeHostStart(JNIEnv *, jclass, jint seed) {
    g_startSeed = seed;
    g_gameSeed = seed;
    g_hostNR = -1;
    g_gHave = 0;
    g_animReset = 1;
    g_startAnnounce = 1;
    g_startReq = 1;
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeJoinGame(JNIEnv *, jclass, jint seed, jint nr) {
    g_startSeed = seed;
    g_gameSeed = seed;
    g_hostNR = nr;
    g_gHave = 0;
    g_animReset = 1;
    g_startAnnounce = 0;
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

static volatile int g_role = 0; // 0 = tek kisilik, 1 = host, 2 = istemci
static bool g_role_for_block() { return g_role != 0; }

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
    int prev = g_blockGranny;
    const char *pc = g_ctx;
    if (g_role_for_block()) {
        g_blockGranny = 1;
        g_ctx = "yeni gun";
    }
    bool r = orig_DayAds(self, mi);
    g_blockGranny = prev;
    g_ctx = pc;
    return r;
}

// --- PlayerPrefs.GetInt(string,int) gozlemi: hangi kayitli ayarlar okunuyor? ---
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

// --- Granny senkronu: host'ta AI calisir ve konumu/animasyonu yayilir, istemcide AI kapatilir ---
typedef void (*SetVecFn)(intptr_t self, const Vec3 *v, const void *mi);
typedef void (*SetQuatFn)(intptr_t self, const Quat *q, const void *mi);
typedef void (*SetEnabledFn)(void *self, bool v, const void *mi);
typedef void (*CrossFadeFn)(void *self, void *name, float fade, int mode, const void *mi);
typedef bool (*PlayFn)(void *self, void *name, int mode, const void *mi);
typedef void *(*GetGoFn)(void *comp, const void *mi);
typedef void *(*GetCompStrFn)(void *go, void *typeStr, const void *mi);
static UpdateFn orig_GrannyFixed = nullptr;
static CrossFadeFn orig_CrossFade = nullptr;
static PlayFn orig_Play = nullptr;
static SetVecFn orig_SetPos = nullptr;
static const uintptr_t RVA_GRANNY_FIXED = 0x14549E4;    // EnemyAIGranny.FixedUpdate
static const uintptr_t RVA_SET_POS = 0x2AA91EC;         // Transform.set_position_Injected
static const uintptr_t RVA_SET_ROT = 0x2AA988C;         // Transform.set_rotation_Injected
static const uintptr_t RVA_BEH_SET_ENABLED = 0x2A99DB4; // Behaviour.set_enabled
static const uintptr_t RVA_CROSSFADE3 = 0x2A607BC;      // Animation.CrossFade(string,float,PlayMode)
static const uintptr_t RVA_PLAY2 = 0x2A60594;           // Animation.Play(string,PlayMode)
static const uintptr_t RVA_CAUGHT_MOVE = 0x145C658;     // EnemyAIGranny.<Playercaught>.MoveNext
static const uintptr_t RVA_GET_GAMEOBJECT = 0x2A9A678;  // Component.get_gameObject
static const uintptr_t RVA_GO_GETCOMP_STR = 0x2A9D5C8;  // GameObject.GetComponent(string)
static const uintptr_t OFF_GRANNY_TRANSFORM = 0x28;     // EnemyAIGranny.myTransform
static const uintptr_t OFF_GRANNY_NAV = 0x108;          // EnemyAIGranny.navComponent
static const uintptr_t OFF_GRANNY_ANIMHOLDER = 0x1D8;   // EnemyAIGranny.animationHolder
static float g_gx = 0, g_gy = 0, g_gz = 0, g_gyaw = 0;
static int g_gTick = 0;
static void *g_navFor = nullptr;

static void *il2_str(const char *s) {
    static StringNewFn sn = nullptr;
    if (!sn) {
        void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) sn = (StringNewFn) dlsym(h, "il2cpp_string_new");
    }
    return sn ? sn(s) : nullptr;
}

// Animasyon olaylari kuyrugu (Java -> ana is parcacigi)
struct AnimEv {
    char name[48];
    float fade;
};
static AnimEv g_animQ[8];
static int g_animHead = 0, g_animTail = 0;
static pthread_mutex_t g_animMx = PTHREAD_MUTEX_INITIALIZER;

static void anim_push(const char *n, float f) {
    pthread_mutex_lock(&g_animMx);
    int next = (g_animHead + 1) % 8;
    if (next != g_animTail) {
        strncpy(g_animQ[g_animHead].name, n, 47);
        g_animQ[g_animHead].name[47] = 0;
        g_animQ[g_animHead].fade = f;
        g_animHead = next;
    }
    pthread_mutex_unlock(&g_animMx);
}

static bool anim_pop(AnimEv *out) {
    pthread_mutex_lock(&g_animMx);
    bool ok = g_animTail != g_animHead;
    if (ok) {
        *out = g_animQ[g_animTail];
        g_animTail = (g_animTail + 1) % 8;
    }
    pthread_mutex_unlock(&g_animMx);
    return ok;
}

static bool is_granny_anim(void *anim) {
    if (!g_grannySelf || !anim) return false;
    GetGoFn getGo = (GetGoFn) (g_base + RVA_GET_GAMEOBJECT);
    void *go = getGo(anim, nullptr);
    if (!go) return false;
    void *holder = *(void **) ((uintptr_t) g_grannySelf + OFF_GRANNY_ANIMHOLDER);
    if (holder && go == holder) return true;
    return go == getGo(g_grannySelf, nullptr);
}

static int g_animLogs = 0;
static char g_lastGranny[48] = {0};
static int64_t g_lastGrannyMs = 0;

static int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Dunya animasyonlari (kapilar vb.) icin tekrar engelleme tablosu
struct SentEnt {
    uint32_t hash;
    int64_t ms;
};
static SentEnt g_sent[16];
static int g_sentPos = 0;

static uint32_t hash_anim(const char *name, const Vec3 &p) {
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t) *c) * 16777619u;
    int qx = (int) floorf(p.x * 2.0f), qy = (int) floorf(p.y * 2.0f), qz = (int) floorf(p.z * 2.0f);
    h = (h ^ (uint32_t) qx) * 16777619u;
    h = (h ^ (uint32_t) qy) * 16777619u;
    h = (h ^ (uint32_t) qz) * 16777619u;
    return h;
}

typedef void *(*GetTrFn)(void *comp, const void *mi);
static const uintptr_t RVA_GET_TRANSFORM = 0x2A9A5B0;   // Component.get_transform

static void forward_anim(void *anim, void *nameStr, float fade) {
    if (g_role == 0 || !anim) return;
    if (g_animReset) {
        g_animReset = 0;
        g_lastGranny[0] = 0;
        memset(g_sent, 0, sizeof(g_sent));
    }
    char nm[48];
    il2str_ascii(nameStr, nm, sizeof(nm));
    char b[96];
    JNIEnv *env = env_for_thread();
    if (!env || !g_modClass) return;
    if (is_granny_anim(anim)) {
        if (g_role != 1 || !g_onGrannyAnim) return;
        int64_t t = now_ms();
        if (strcmp(nm, g_lastGranny) == 0 && t - g_lastGrannyMs < 4000) return;
        strncpy(g_lastGranny, nm, sizeof(g_lastGranny) - 1);
        g_lastGrannyMs = t;
        if (g_animLogs < 6) {
            g_animLogs++;
            snprintf(b, sizeof(b), "granny anim: %s (%.2f)", nm, fade);
            jlog(b);
        }
        jstring js = env->NewStringUTF(nm);
        env->CallStaticVoidMethod(g_modClass, g_onGrannyAnim, js, fade);
        env->DeleteLocalRef(js);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    // Dunya animasyonu (kapi, cekmece, tuzak...): konumla birlikte herkese yayilir
    if (!g_onWorldAnim) return;
    void *tr = ((GetTrFn) (g_base + RVA_GET_TRANSFORM))(anim, nullptr);
    if (!tr) return;
    intptr_t cached = *(intptr_t *) ((uintptr_t) tr + OFF_CACHED_PTR);
    if (!cached) return;
    Vec3 p = {0, 0, 0};
    ((GetVec3Fn) (g_base + RVA_GET_POS))(cached, &p, nullptr);
    uint32_t h = hash_anim(nm, p);
    int64_t t = now_ms();
    for (int i = 0; i < 16; i++) {
        if (g_sent[i].hash == h && t - g_sent[i].ms < 1500) return;
    }
    g_sent[g_sentPos].hash = h;
    g_sent[g_sentPos].ms = t;
    g_sentPos = (g_sentPos + 1) % 16;
    jstring js = env->NewStringUTF(nm);
    env->CallStaticVoidMethod(g_modClass, g_onWorldAnim, js, fade, p.x, p.y, p.z);
    env->DeleteLocalRef(js);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Olum/yeni gun akisinda kapi-cekmece "kapanma" animasyonlarini engelle (ve kaydet)
static bool name_has_clos(const char *n) {
    char low[48];
    int i = 0;
    for (; n[i] && i < 47; i++) low[i] = (n[i] >= 'A' && n[i] <= 'Z') ? (char) (n[i] + 32) : n[i];
    low[i] = 0;
    return strstr(low, "clos") != nullptr;
}

static bool death_block_anim(void *anim, void *nameStr) {
    if (!g_blockGranny || g_role == 0) return false;
    char nm[48];
    il2str_ascii(nameStr, nm, sizeof(nm));
    if (is_granny_anim(anim)) return false;
    bool blk = name_has_clos(nm);
    static int logs = 0;
    if (logs < 20) {
        logs++;
        char b[110];
        snprintf(b, sizeof(b), "olum anim: %s [%s]", nm, blk ? "engellendi" : "gecti");
        jlog(b);
    }
    return blk;
}

static void hook_CrossFade(void *self, void *name, float fade, int mode, const void *mi) {
    if (death_block_anim(self, name)) return;
    orig_CrossFade(self, name, fade, mode, mi);
    forward_anim(self, name, fade);
}

static bool hook_Play(void *self, void *name, int mode, const void *mi) {
    if (death_block_anim(self, name)) return false;
    bool r = orig_Play(self, name, mode, mi);
    forward_anim(self, name, -1.0f);
    return r;
}

static void hook_SetPos(intptr_t self, const Vec3 *v, const void *mi) {
    if (g_role != 0 && g_grannyCached && self == g_grannyCached) {
        static int logs = 0;
        char b[120];
        if (g_blockGranny) {
            if (logs < 12) {
                logs++;
                snprintf(b, sizeof(b), "Granny isinlanmasi engellendi [%s]", g_ctx ? g_ctx : "?");
                jlog(b);
            }
            return;
        }
        if (logs < 12) {
            logs++;
            snprintf(b, sizeof(b), "Granny konumu degisti (engel yok) %.1f %.1f %.1f", v->x, v->y, v->z);
            jlog(b);
        }
    }
    if (orig_SetPos) orig_SetPos(self, v, mi);
}

// Yonetilen Transform.set_position(Vector3): enjekte surumu icine gomuldugu icin oyun kodu bunu dogrudan cagirir
typedef void (*SetPosMFn)(void *self, Vec3 v, const void *mi);
static SetPosMFn orig_SetPosM = nullptr;
static const uintptr_t RVA_SET_POS_M = 0x2AA9164;   // Transform.set_position(Vector3)

static void hook_SetPosM(void *self, Vec3 v, const void *mi) {
    if (g_role != 0 && g_grannyCached && self) {
        intptr_t c = *(intptr_t *) ((uintptr_t) self + OFF_CACHED_PTR);
        if (c == g_grannyCached) {
            static int logsBlocked = 0, logsFree = 0;
            char b[128];
            if (g_blockGranny) {
                if (logsBlocked < 12) {
                    logsBlocked++;
                    snprintf(b, sizeof(b), "Granny isinlanmasi engellendi [%s]", g_ctx ? g_ctx : "?");
                    jlog(b);
                }
                return;
            }
            if (logsFree < 6) {
                logsFree++;
                snprintf(b, sizeof(b), "Granny konumu degisti (engel yok) %.1f %.1f %.1f", v.x, v.y, v.z);
                jlog(b);
            }
        }
    }
    if (orig_SetPosM) orig_SetPosM(self, v, mi);
}

// Olum / yeni gun akisindaki coroutine'lerde Granny'nin isinlanmasini engelle
#define DEATH_HOOK(NAME, TAG) \
    static MoveNextFn orig_##NAME = nullptr; \
    static bool hook_##NAME(void *self, const void *mi) { \
        int prev = g_blockGranny; \
        const char *pc = g_ctx; \
        if (g_role != 0) { g_blockGranny = 1; g_ctx = TAG; } \
        bool r = orig_##NAME(self, mi); \
        g_blockGranny = prev; \
        g_ctx = pc; \
        return r; \
    }

DEATH_HOOK(CaughtMove, "yakalandi")
DEATH_HOOK(EndDay, "gun sonu")
DEATH_HOOK(UnderBed, "yatak alti")
DEATH_HOOK(EndScene3, "bitis sahnesi 3")
DEATH_HOOK(EndScene, "bitis sahnesi")

static UpdateFn orig_SpiderReset = nullptr;
static void hook_SpiderReset(void *self, const void *mi) {
    int prev = g_blockGranny;
    const char *pc = g_ctx;
    if (g_role != 0) {
        g_blockGranny = 1;
        g_ctx = "orumcek";
    }
    orig_SpiderReset(self, mi);
    g_blockGranny = prev;
    g_ctx = pc;
}


typedef void *(*GetStateFn)(void *anim, void *name, const void *mi);
typedef void (*SetSpeedFn)(void *state, float v, const void *mi);
static const uintptr_t RVA_ANIM_GETITEM = 0x2A60310;       // Animation.get_Item(string)
static const uintptr_t RVA_ANIMSTATE_SETSPEED = 0x2A60EE8; // AnimationState.set_speed
static void *g_animComp = nullptr;
static void *g_animCompFor = nullptr;

static void client_apply_anims(void *self) {
    AnimEv ev;
    while (anim_pop(&ev)) {
        if (g_animCompFor != self) {
            g_animCompFor = self;
            g_animComp = nullptr;
            void *tname = il2_str("Animation");
            GetCompStrFn gc = (GetCompStrFn) (g_base + RVA_GO_GETCOMP_STR);
            void *holder = *(void **) ((uintptr_t) self + OFF_GRANNY_ANIMHOLDER);
            if (holder && tname) g_animComp = gc(holder, tname, nullptr);
            if (!g_animComp && tname) {
                void *go = ((GetGoFn) (g_base + RVA_GET_GAMEOBJECT))(self, nullptr);
                if (go) g_animComp = gc(go, tname, nullptr);
            }
            jlog(g_animComp ? "Granny animasyon bileseni bulundu" : "Granny animasyon bileseni YOK");
        }
        if (!g_animComp) continue;
        void *nm = il2_str(ev.name);
        if (!nm) continue;
        if (ev.fade < 0) {
            if (orig_Play) orig_Play(g_animComp, nm, 0, nullptr);
        } else if (orig_CrossFade) {
            orig_CrossFade(g_animComp, nm, ev.fade, 0, nullptr);
        }
        // Istemcide yapay zeka kapali oldugu icin animasyon hizi 0'da kalabiliyor: 1'e esitle
        void *st = ((GetStateFn) (g_base + RVA_ANIM_GETITEM))(g_animComp, nm, nullptr);
        if (st) ((SetSpeedFn) (g_base + RVA_ANIMSTATE_SETSPEED))(st, 1.0f, nullptr);
    }
}

static void hook_GrannyFixed(void *self, const void *mi) {
    if (self) {
        g_grannySelf = self;
        void *tr0 = *(void **) ((uintptr_t) self + OFF_GRANNY_TRANSFORM);
        if (tr0) g_grannyCached = *(intptr_t *) ((uintptr_t) tr0 + OFF_CACHED_PTR);
    }
    if (g_role != 2) {
        orig_GrannyFixed(self, mi);
        if (g_role == 1 && self && (++g_gTick % 5) == 0) {
            if (!g_grannyCached) return;
            Vec3 p = {0, 0, 0};
            Quat q = {0, 0, 0, 1};
            ((GetVec3Fn) (g_base + RVA_GET_POS))(g_grannyCached, &p, nullptr);
            ((GetQuatFn) (g_base + RVA_GET_ROT))(g_grannyCached, &q, nullptr);
            float yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x)) * 57.29578f;
            JNIEnv *env = env_for_thread();
            if (env && g_modClass && g_onGranny) {
                env->CallStaticVoidMethod(g_modClass, g_onGranny, p.x, p.y, p.z, yaw);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
        }
        return;
    }
    // Istemci: yapay zekayi calistirma, host'tan gelen konumu ve animasyonlari uygula
    if (!self) return;
    if (g_navFor != self) {
        g_navFor = self;
        void *nav = *(void **) ((uintptr_t) self + OFF_GRANNY_NAV);
        if (nav) {
            ((SetEnabledFn) (g_base + RVA_BEH_SET_ENABLED))(nav, false, nullptr);
            jlog("Granny: istemci modu (yol bulma kapatildi)");
        }
    }
    client_apply_anims(self);
    if (!g_gHave || !g_grannyCached) return;
    Vec3 p = {g_gx, g_gy, g_gz};
    float h = g_gyaw * 0.01745329f * 0.5f;
    Quat q = {0, sinf(h), 0, cosf(h)};
    SetVecFn sp = orig_SetPos ? orig_SetPos : (SetVecFn) (g_base + RVA_SET_POS);
    sp(g_grannyCached, &p, nullptr);
    ((SetQuatFn) (g_base + RVA_SET_ROT))(g_grannyCached, &q, nullptr);
}

// --- Dunya animasyonlari (kapilar vb.): alici tarafta en yakin Animation bileseni bulunup oynatilir ---
struct WAnim {
    char name[48];
    float fade, x, y, z;
};
static WAnim g_wq[8];
static int g_wHead = 0, g_wTail = 0;
static pthread_mutex_t g_wMx = PTHREAD_MUTEX_INITIALIZER;

static void wanim_push(const char *n, float f, float x, float y, float z) {
    pthread_mutex_lock(&g_wMx);
    int next = (g_wHead + 1) % 8;
    if (next != g_wTail) {
        strncpy(g_wq[g_wHead].name, n, 47);
        g_wq[g_wHead].name[47] = 0;
        g_wq[g_wHead].fade = f;
        g_wq[g_wHead].x = x;
        g_wq[g_wHead].y = y;
        g_wq[g_wHead].z = z;
        g_wHead = next;
    }
    pthread_mutex_unlock(&g_wMx);
}

static bool wanim_pop(WAnim *out) {
    pthread_mutex_lock(&g_wMx);
    bool ok = g_wTail != g_wHead;
    if (ok) {
        *out = g_wq[g_wTail];
        g_wTail = (g_wTail + 1) % 8;
    }
    pthread_mutex_unlock(&g_wMx);
    return ok;
}

typedef void *(*DomainGetFn)();
typedef void **(*DomainAsmsFn)(void *domain, size_t *size);
typedef void *(*AsmImageFn)(void *asm_);
typedef const char *(*ImageNameFn)(void *image);
typedef void *(*ClassFromNameFn)(void *image, const char *ns, const char *name);
typedef void *(*ClassTypeFn)(void *klass);
typedef void *(*TypeObjFn)(void *type);
typedef void *(*FindObjsFn)(void *type, const void *mi);
static const uintptr_t RVA_FIND_OBJECTS = 0x2AA37A8;   // Object.FindObjectsOfType(Type)

static void *il2_type_object(const char *imageName, const char *ns, const char *cls) {
    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return nullptr;
    DomainGetFn dget = (DomainGetFn) dlsym(h, "il2cpp_domain_get");
    DomainAsmsFn dasm = (DomainAsmsFn) dlsym(h, "il2cpp_domain_get_assemblies");
    AsmImageFn aimg = (AsmImageFn) dlsym(h, "il2cpp_assembly_get_image");
    ImageNameFn iname = (ImageNameFn) dlsym(h, "il2cpp_image_get_name");
    ClassFromNameFn cfn = (ClassFromNameFn) dlsym(h, "il2cpp_class_from_name");
    ClassTypeFn ctype = (ClassTypeFn) dlsym(h, "il2cpp_class_get_type");
    TypeObjFn tobj = (TypeObjFn) dlsym(h, "il2cpp_type_get_object");
    if (!dget || !dasm || !aimg || !iname || !cfn || !ctype || !tobj) return nullptr;
    size_t n = 0;
    void **asms = dasm(dget(), &n);
    for (size_t i = 0; i < n; i++) {
        void *img = aimg(asms[i]);
        const char *nm = img ? iname(img) : nullptr;
        if (nm && strcmp(nm, imageName) == 0) {
            void *k = cfn(img, ns, cls);
            if (!k) return nullptr;
            return tobj(ctype(k));
        }
    }
    return nullptr;
}

static void *g_animTypeObj = nullptr;
static bool g_animTypeTried = false;

static void process_world_anims() {
    WAnim ev;
    while (wanim_pop(&ev)) {
        if (g_role == 0) continue;
        if (!g_animTypeObj && !g_animTypeTried) {
            g_animTypeTried = true;
            g_animTypeObj = il2_type_object("UnityEngine.AnimationModule.dll", "UnityEngine", "Animation");
            jlog(g_animTypeObj ? "Animasyon tipi bulundu" : "Animasyon tipi YOK");
        }
        if (!g_animTypeObj || !orig_Play || !orig_CrossFade) continue;
        void *arr = ((FindObjsFn) (g_base + RVA_FIND_OBJECTS))(g_animTypeObj, nullptr);
        if (!arr) continue;
        int64_t len = *(int64_t *) ((uintptr_t) arr + 0x18);
        void **items = (void **) ((uintptr_t) arr + 0x20);
        void *best = nullptr;
        float bd = 1e9f;
        for (int64_t i = 0; i < len && i < 512; i++) {
            void *comp = items[i];
            if (!comp || is_granny_anim(comp)) continue;
            void *tr = ((GetTrFn) (g_base + RVA_GET_TRANSFORM))(comp, nullptr);
            if (!tr) continue;
            intptr_t c = *(intptr_t *) ((uintptr_t) tr + OFF_CACHED_PTR);
            if (!c) continue;
            Vec3 p = {0, 0, 0};
            ((GetVec3Fn) (g_base + RVA_GET_POS))(c, &p, nullptr);
            float dx = p.x - ev.x, dy = p.y - ev.y, dz = p.z - ev.z;
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < bd) {
                bd = d;
                best = comp;
            }
        }
        static int wlogs = 0;
        if (best && bd < 2.0f) {
            void *nm = il2_str(ev.name);
            if (nm) {
                if (ev.fade < 0) orig_Play(best, nm, 0, nullptr);
                else orig_CrossFade(best, nm, ev.fade, 0, nullptr);
            }
            if (wlogs < 8) {
                wlogs++;
                char b[96];
                snprintf(b, sizeof(b), "dunya anim: %s (mesafe %.1f)", ev.name, bd);
                jlog(b);
            }
        } else if (wlogs < 8) {
            wlogs++;
            char b[96];
            snprintf(b, sizeof(b), "dunya anim: %s nesne bulunamadi (en yakin %.1f)", ev.name, bd);
            jlog(b);
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeWorldAnim(JNIEnv *env, jclass, jstring name, jfloat fade, jfloat x, jfloat y, jfloat z) {
    if (!name) return;
    const char *c = env->GetStringUTFChars(name, nullptr);
    if (c) {
        wanim_push(c, fade, x, y, z);
        env->ReleaseStringUTFChars(name, c);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeSetRole(JNIEnv *, jclass, jint role) {
    g_role = role;
    if (role != 2) g_navFor = nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeSetGranny(JNIEnv *, jclass, jfloat x, jfloat y, jfloat z, jfloat yaw) {
    g_gx = x;
    g_gy = y;
    g_gz = z;
    g_gyaw = yaw;
    g_gHave = 1;
}

extern "C" JNIEXPORT void JNICALL
Java_com_dvloper_mod_Mod_nativeGrannyAnim(JNIEnv *env, jclass, jstring name, jfloat fade) {
    if (!name) return;
    const char *c = env->GetStringUTFChars(name, nullptr);
    if (c) {
        anim_push(c, fade);
        env->ReleaseStringUTFChars(name, c);
    }
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
        static const uint32_t expGr[4] = {0xd10303ff, 0x6d033bef, 0x6d0433ed, 0x6d052beb};
        void *o9 = nullptr;
        int r9 = inline_hook((void *) (base + RVA_GRANNY_FIXED), (void *) hook_GrannyFixed, &o9, expGr);
        if (r9 == 0) orig_GrannyFixed = (UpdateFn) o9;
        char gb[64];
        snprintf(gb, sizeof(gb), "Granny hook: %d (0=tamam)", r9);
        jlog(gb);
        static const uint32_t expCf[4] = {0xd10183ff, 0xfd001be8, 0xf9001ffe, 0xa90457f6};
        static const uint32_t expPl[4] = {0xd10183ff, 0xf9001bfe, 0xa90457f6, 0xa9054ff4};
        static const uint32_t expSp[4] = {0xa9be57fe, 0xa9014ff4, 0xf00031b5, 0xaa0103f3};
        static const uint32_t expCt[4] = {0xfc1b0fea, 0x6d00a3e9, 0xf9000ffe, 0xa9025ff8};
        void *oa = nullptr, *ob = nullptr, *oc = nullptr, *od = nullptr;
        int ra = inline_hook((void *) (base + RVA_CROSSFADE3), (void *) hook_CrossFade, &oa, expCf);
        int rb = inline_hook((void *) (base + RVA_PLAY2), (void *) hook_Play, &ob, expPl);
        int rc = inline_hook((void *) (base + RVA_SET_POS), (void *) hook_SetPos, &oc, expSp);
        int rd = inline_hook((void *) (base + RVA_CAUGHT_MOVE), (void *) hook_CaughtMove, &od, expCt);
        if (ra == 0) orig_CrossFade = (CrossFadeFn) oa;
        if (rb == 0) orig_Play = (PlayFn) ob;
        if (rc == 0) orig_SetPos = (SetVecFn) oc;
        if (rd == 0) orig_CaughtMove = (MoveNextFn) od;
        char ab[96];
        snprintf(ab, sizeof(ab), "Animasyon hook: cf=%d play=%d setpos=%d yakalandi=%d (0=tamam)", ra, rb, rc, rd);
        jlog(ab);
        static const uint32_t expE1[4] = {0xa9bc67fe, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
        static const uint32_t expE2[4] = {0xa9bd5ffe, 0xa90157f6, 0xa9024ff4, 0xd000dcb3};
        static const uint32_t expE3[4] = {0xa9bd5ffe, 0xa90157f6, 0xa9024ff4, 0xf000dc94};
        static const uint32_t expE4[4] = {0xf81e0ffe, 0xa9014ff4, 0xf000dfb4, 0xaa0003f3};
        void *p1 = nullptr, *p2 = nullptr, *p3 = nullptr, *p4 = nullptr, *p5 = nullptr;
        int q1 = inline_hook((void *) (base + 0x1516AB0), (void *) hook_EndDay, &p1, expE1);
        int q2 = inline_hook((void *) (base + 0x1542118), (void *) hook_UnderBed, &p2, expE2);
        int q3 = inline_hook((void *) (base + 0x15452E8), (void *) hook_EndScene3, &p3, expE3);
        int q4 = inline_hook((void *) (base + 0x1545B38), (void *) hook_EndScene, &p4, expE3);
        int q5 = inline_hook((void *) (base + 0x14E1A14), (void *) hook_SpiderReset, &p5, expE4);
        if (q1 == 0) orig_EndDay = (MoveNextFn) p1;
        if (q2 == 0) orig_UnderBed = (MoveNextFn) p2;
        if (q3 == 0) orig_EndScene3 = (MoveNextFn) p3;
        if (q4 == 0) orig_EndScene = (MoveNextFn) p4;
        if (q5 == 0) orig_SpiderReset = (UpdateFn) p5;
        char olumBuf[96];
        snprintf(olumBuf, sizeof(olumBuf), "Olum hook: %d %d %d %d %d (0=tamam)", q1, q2, q3, q4, q5);
        jlog(olumBuf);
        static const uint32_t expSm[4] = {0xd100c3ff, 0xf9000bfe, 0xa9024ff4, 0xf00031b4};
        void *osm = nullptr;
        int rsm = inline_hook((void *) (base + RVA_SET_POS_M), (void *) hook_SetPosM, &osm, expSm);
        if (rsm == 0) orig_SetPosM = (SetPosMFn) osm;
        char smBuf[64];
        snprintf(smBuf, sizeof(smBuf), "SetPos(yonetilen) hook: %d (0=tamam)", rsm);
        jlog(smBuf);
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
    g_onHostStart = env->GetStaticMethodID(c, "onHostStartReady", "(I)V");
    g_onGranny = env->GetStaticMethodID(c, "onGrannyLocal", "(FFFF)V");
    g_onGrannyAnim = env->GetStaticMethodID(c, "onGrannyAnim", "(Ljava/lang/String;F)V");
    g_onWorldAnim = env->GetStaticMethodID(c, "onWorldAnim", "(Ljava/lang/String;FFFF)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    pthread_t t;
    pthread_create(&t, nullptr, init_thread, nullptr);
    pthread_detach(t);
    return JNI_VERSION_1_6;
}

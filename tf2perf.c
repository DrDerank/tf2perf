// tf2perf.c - TF2 x64 client-side performance hooks
// Loads into hl2.exe (TF2 x64), installs MinHook detours on hot client paths,
// and serves a control CLI over the named pipe \\.\pipe\tf2perf.
//
// Build: build.cmd  (MSVC x64, links C:\mh\lib\libMinHook.x64.lib)
//
// Hooks:
//   CViewRender::RenderView                      - frame boundary / budget reset
//   TF_3rdPersonMuzzleFlashCallback              - per-frame sprite budget
//   TF_3rdPersonMuzzleFlashCallback_SentryGun    - same budget
//   CParticleMgr per-effect simulate (0x2D82E0)  - per-frame sim budget
//   C_BaseAnimating::UpdateClientSideAnimations  - frame throttle
//   CClientShadowMgr::ComputeShadowDepthTextures - shadow pass kill switch
//   cl_particle_retire_cost (no hook)            - writes the cheat-flagged cvar
//
// All targets are pattern-scanned; every one is verified before patching.
// Log: %TEMP%\tf2perf.log

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"

#define PIPE_NAME   "\\\\.\\pipe\\tf2perf"
#define END_MARK    "###END###"

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static FILE*        g_log = NULL;
static CRITICAL_SECTION g_log_cs;
static int          g_log_ready = 0;

static void logf_(const char* fmt, ...)
{
    va_list ap;
    if (!g_log_ready) return;
    EnterCriticalSection(&g_log_cs);
    if (g_log)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------------------------------------------------------
// settings (touched by pipe thread, read by game thread)
// ---------------------------------------------------------------------------
static volatile LONG g_muzzleflash_cap = 0;   // 0 = off, else max flashes per frame
static volatile LONG g_sim_budget      = 0;   // 0 = off, else max particle effects simulated per frame
static volatile LONG g_anim_throttle   = 1;   // 1 = every frame, N = run every Nth frame (CAUSES MODEL TWITCH)
static volatile LONG g_anim_dist       = 0;   // units; 0 = off. Skips animation updates for entities beyond N
// client-side animation list globals (RVAs from IDA; validated at init before use)
static uintptr_t g_animlist_ptr_addr   = 0;
static uintptr_t g_animlist_count_addr = 0;
static int       g_animlist_ok         = 0;
static volatile LONG g_shadows_on      = 1;   // 0 = skip ComputeShadowDepthTextures
static volatile LONG g_retire_cost_set = 0;   // 1 = retire value has been applied
static float         g_retire_cost     = 0.0f;
static volatile LONG g_hud_throttle    = 1;   // 1 = paint HUD every frame, N = every Nth frame
static volatile LONG g_vguisim_throttle = 1;  // 1 = simulate VGUI every frame, N = every Nth frame
static volatile LONG g_far_dist        = 0;   // units; 0 = off (brush models, temp-ent models)
static volatile LONG g_far_bones_dist  = 0;   // units; 0 = off (SetupBones gate, experimental)
static volatile LONG g_far_weapons     = 0;   // units; 0 = off (weapon world-model cull, opt-in:
                                              // weapon origins are unreliable, can cull everything)
static volatile LONG g_menuoff         = 1;   // 1 = suspend all settings outside a match
                                              // (needs both engine signals to agree)
static volatile LONG g_in_game         = 1;   // updated from the engine each frame
static int           g_menu_saved      = 0;
static LONG g_sv_mf, g_sv_sim, g_sv_anim, g_sv_animth, g_sv_far, g_sv_farb, g_sv_farw, g_sv_shadows, g_sv_vguisim, g_sv_hud;
static float         g_view_origin[3]  = { 0.0f, 0.0f, 0.0f };

// ---------------------------------------------------------------------------
// counters
// ---------------------------------------------------------------------------
static volatile LONG g_frame        = 0;
static volatile LONG g_mf_count     = 0, g_mf_total = 0, g_mf_skipped = 0;
static volatile LONG g_sim_count    = 0, g_sim_total = 0, g_sim_skipped = 0;
static volatile LONG g_anim_runs    = 0, g_anim_skips = 0;
static volatile LONG g_shadow_calls = 0, g_shadow_skips = 0;
static volatile LONG g_vguipaint_calls = 0, g_vguipaint_skips = 0;
static volatile LONG g_vguisim_calls = 0, g_vguisim_skips = 0;
static volatile LONG g_shadowpre_calls = 0, g_shadowpre_skips = 0;
static volatile LONG g_brush_calls = 0, g_brush_skips = 0;
static volatile LONG g_weapon_calls = 0, g_weapon_skips = 0;
static volatile LONG g_tempmodel_calls = 0, g_tempmodel_skips = 0;
static volatile LONG g_bones_calls = 0, g_bones_skips = 0;

// ---------------------------------------------------------------------------
// resolved targets
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    const char* module;
    const char* sig;
    void*       addr;      // resolved function
    void*       detour;
    void**      orig;
    uint8_t*    modbase;
    int         hooked;
} target_t;

static target_t g_targets[] = {
    { "CViewRender::RenderView", "client.dll",
      "48 8B C4 44 89 48 ? 44 89 40 ? 48 89 50 ? 48 89 48 ? 55 53",
      NULL, NULL, NULL, NULL, 0 },

    { "TF_3rdPersonMuzzleFlashCallback", "client.dll",
      "4C 8B DC 48 81 EC C8 00 00 00 8B 41",
      NULL, NULL, NULL, NULL, 0 },

    { "TF_3rdPersonMuzzleFlash_SentryGun", "client.dll",
      "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 30 8B 71",
      NULL, NULL, NULL, NULL, 0 },

    { "CParticleMgr::SimulateParticleEffect", "client.dll",
      "48 89 5C 24 ? 57 48 83 EC 20 48 8B 19 0F B6 79",
      NULL, NULL, NULL, NULL, 0 },

    { "C_BaseAnimating::UpdateClientSideAnimations", "client.dll",
      "4C 8B DC 49 89 5B ? 49 89 6B ? 56 57 41 56 48 83 EC 60 48 8B 05 ? ? ? ? "
      "48 8D 1D ? ? ? ? 33 ED 48 8D 3D ? ? ? ? 49 89 6B ? 4C 8B 50 ? 4D 85 D2 74 ? "
      "49 89 5B ? 48 8D 05 ? ? ? ? 49 89 7B ? 49 8D 53 ? 49 89 43 ? 45 33 C9 48 8D 05 ? ? ? ? "
      "45 33 C0 49 89 43 ? 49 8B CA 48 8D 05 ? ? ? ? C7 44 24 ? ? ? ? ? 49 89 43 ? 49 89 6B ? "
      "41 FF 92 ? ? ? ? 48 8B 05 ? ? ? ? 48 8B AC 24 ? ? ? ? 48 8B 0D ? ? ? ? 48 8B 70 ? "
      "44 8B B1 ? ? ? ? 45 85 F6 74 ? C7 44 24 ? ? ? ? ? 4C 8B CF 45 33 C0 C6 44 24 ? ? "
      "48 8B D3 FF 15 ? ? ? ? 48 8B 0D",
      NULL, NULL, NULL, NULL, 0 },

    { "CClientShadowMgr::ComputeShadowDepthTextures", "client.dll",
      "48 8B C4 55 48 8D A8 ? ? ? ? 48 81 EC A0 09 00 00",
      NULL, NULL, NULL, NULL, 0 },

    { "CEngineVGui::Paint", "engine.dll",
      "4C 8B DC 41 54 41 57 48 81 EC A8 00 00 00",
      NULL, NULL, NULL, NULL, 0 },

    { "CEngineVGui::Simulate", "engine.dll",
      "41 57 48 81 EC A0 00 00 00 4C 8B F9",
      NULL, NULL, NULL, NULL, 0 },

    { "CClientShadowMgr::PreRender", "client.dll",
      "4C 8B DC 49 89 5B ? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 8B 05 ? ? ? ? 48 8D 35",
      NULL, NULL, NULL, NULL, 0 },

    { "C_BaseEntity::DrawBrushModel", "client.dll",
      "4C 8B DC 49 89 5B ? 49 89 6B ? 49 89 73 ? 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 8B 2D",
      NULL, NULL, NULL, NULL, 0 },

    { "C_BaseCombatWeapon::DrawModel", "client.dll",
      "4C 8B DC 49 89 5B ? 49 89 6B ? 56 57 41 54 41 56 41 57 48 83 EC 60 48 8B 2D ? ? ? ? 48 8D 1D",
      NULL, NULL, NULL, NULL, 0 },

    { "C_LocalTempEntity::DrawStudioModel", "client.dll",
      "4C 8B DC 49 89 5B ? 89 54 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70",
      NULL, NULL, NULL, NULL, 0 },

    { "C_BaseAnimating::SetupBones", "client.dll",
      "48 8B C4 44 89 40 ? 48 89 50 ? 55 53",
      NULL, NULL, NULL, NULL, 0 },
};

#define NUM_TARGETS (sizeof(g_targets) / sizeof(g_targets[0]))

// cl_particle_retire_cost: locate the m_pParent field of the ConVar object by the
// instruction sequence in EarlyRetireParticleSystems, then the value lives at
// ConVar+0x18 (name, validated), +0x54 (float value), +0x58 (int value).
#define RETIRE_SIG "48 8B 15 ? ? ? ? 0F 57 C0 F3 0F 10 7A 54 F3 0F 59 3D"
static void** g_retire_cvar_parent_field = NULL;
static void*  g_retire_cvar = NULL;

// ---------------------------------------------------------------------------
// pattern scanner ("48 8B ? 90" / "??" wildcards, case-insensitive hex)
// ---------------------------------------------------------------------------
typedef struct { uint8_t bytes[512]; uint8_t mask[512]; int len; } pattern_t;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_pattern(const char* sig, pattern_t* out)
{
    int n = 0;
    const char* p = sig;
    while (*p)
    {
        while (*p == ' ') p++;
        if (!*p) break;
        if (n >= (int)sizeof(out->bytes)) return 0;
        if (*p == '?')
        {
            out->bytes[n] = 0; out->mask[n] = 0;
            p++;
            if (*p == '?') p++;
            n++;
        }
        else
        {
            int hi = hexval(p[0]), lo = hexval(p[1]);
            if (hi < 0 || lo < 0) return 0;
            out->bytes[n] = (uint8_t)((hi << 4) | lo);
            out->mask[n] = 0xFF;
            p += 2;
            n++;
        }
    }
    out->len = n;
    return n > 0;
}

static uint8_t* find_pattern(uint8_t* base, size_t size, const char* sig, pattern_t* pp)
{
    if (!parse_pattern(sig, pp)) return NULL;
    if ((size_t)pp->len > size) return NULL;

    uint8_t first = pp->bytes[0];
    uint8_t last  = pp->bytes[pp->len - 1];
    uint8_t fmask = pp->mask[0];
    uint8_t lmask = pp->mask[pp->len - 1];

    size_t limit = size - pp->len;
    for (size_t i = 0; i <= limit; i++)
    {
        if (fmask && base[i] != first) continue;
        if (lmask && base[i + pp->len - 1] != last) continue;
        int ok = 1;
        for (int j = 1; j < pp->len - 1; j++)
        {
            if (pp->mask[j] && base[i + j] != pp->bytes[j]) { ok = 0; break; }
        }
        if (ok) return base + i;
    }
    return NULL;
}

static int get_module_range(const char* name, uint8_t** base, size_t* size)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)h;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)h + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    *base = (uint8_t*)h;
    *size = nt->OptionalHeader.SizeOfImage;
    return 1;
}

// ---------------------------------------------------------------------------
// detours
// ---------------------------------------------------------------------------
static int  engine_in_game(void);
static void menu_apply(int in_game);

typedef void (__fastcall *RenderViewFn)(void*, const void*, int, int);
static RenderViewFn o_RenderView = NULL;
static void __fastcall hk_RenderView(void* self, const void* view, int clearFlags, int whatToDraw)
{
    InterlockedIncrement(&g_frame);
    g_mf_count = 0;
    g_sim_count = 0;
    {
        int ig = engine_in_game();
        if (ig != g_in_game) { g_in_game = ig; menu_apply(ig); }
    }
    if (view)
    {
        // CViewSetup.origin lives at +0x40 (validated against width@+0x10, height@+0x18)
        const float* o = (const float*)((const char*)view + 0x40);
        g_view_origin[0] = o[0];
        g_view_origin[1] = o[1];
        g_view_origin[2] = o[2];
    }
    o_RenderView(self, view, clearFlags, whatToDraw);
}

typedef void (__fastcall *MuzzleFn)(const void*);
static MuzzleFn o_Muzzle = NULL;
static MuzzleFn o_MuzzleSentry = NULL;

static void __fastcall muzzle_common(MuzzleFn orig, const void* data)
{
    // main thread only -> plain increments, no locked ops
    g_mf_total++;
    LONG cap = g_muzzleflash_cap;
    if (cap > 0)
    {
        if (++g_mf_count > cap)
        {
            g_mf_skipped++;
            return;
        }
    }
    orig(data);
}

static void __fastcall hk_Muzzle(const void* data)       { muzzle_common(o_Muzzle, data); }
static void __fastcall hk_MuzzleSentry(const void* data) { muzzle_common(o_MuzzleSentry, data); }

typedef int64_t (__fastcall *SimFn)(void*, void*, void*, void*);
static SimFn o_Sim = NULL;
static int64_t __fastcall hk_Sim(void* entry, void* a2, void* a3, void* a4)
{
    // may run on particle worker threads -> keep interlocked counters
    InterlockedIncrement(&g_sim_total);
    LONG budget = g_sim_budget;
    if (budget > 0)
    {
        if (InterlockedIncrement(&g_sim_count) > budget)
        {
            InterlockedIncrement(&g_sim_skipped);
            return 0;
        }
    }
    return o_Sim(entry, a2, a3, a4);
}

static float ent_dist_sq(void* self);   // defined in the distance-culling section below

typedef void (__fastcall *AnimFn)(void);
static AnimFn o_Anim = NULL;

static void animlist_init(uint8_t* base, size_t size)
{
    g_animlist_ptr_addr   = (uintptr_t)(base + 0x1069F80);
    g_animlist_count_addr = (uintptr_t)(base + 0x1069F90);

    int count = *(int*)g_animlist_count_addr;
    if (count < 0 || count > 20000) { logf_("animlist: count validation failed (%d)", count); return; }
    if (count > 0)
    {
        char* list = *(char**)g_animlist_ptr_addr;
        if (!list) { logf_("animlist: null list with count=%d", count); return; }
        void* p = *(void**)list;
        if (!p) { logf_("animlist: null first entry"); return; }
        void* vt = *(void**)p;
        if ((uint8_t*)vt < base || (uint8_t*)vt >= base + size)
        {
            logf_("animlist: vtable %p outside client.dll, refusing", vt);
            return;
        }
    }
    g_animlist_ok = 1;
    logf_("animlist: validated (count=%d)", count);
}

static void __fastcall hk_Anim(void)
{
    LONG d = g_anim_dist;
    LONG n = g_anim_throttle;

    if (d <= 0)
    {
        // legacy global throttle (twitches models/taunts - prefer animdist)
        if (n > 1 && (g_frame % n) != 0) { g_anim_skips++; return; }
        g_anim_runs++;
        o_Anim();
        return;
    }

    if (!g_animlist_ok) { InterlockedIncrement(&g_anim_runs); o_Anim(); return; }

    int count = *(int*)g_animlist_count_addr;
    char* list = *(char**)g_animlist_ptr_addr;
    if (!list || count <= 0) return;

    float lim2 = (float)d * (float)d;
    for (int i = 0; i < count; i++)
    {
        char* e = list + 16 * i;
        void* p = *(void**)e;
        if (!p || !(e[8] & 1)) continue;
        if (ent_dist_sq(p) > lim2) { g_anim_skips++; continue; }
        ((void (__fastcall *)(void*))(*(void***)p)[194])(p);
        g_anim_runs++;
    }
}

typedef void (__fastcall *ShadowFn)(void*, const void*);
static ShadowFn o_Shadow = NULL;
static void __fastcall hk_Shadow(void* self, const void* view)
{
    InterlockedIncrement(&g_shadow_calls);
    if (!g_shadows_on)
    {
        InterlockedIncrement(&g_shadow_skips);
        return;
    }
    o_Shadow(self, view);
}

// engine.dll: VGUI paint (mode selects which layers) and VGUI simulate
typedef void (__fastcall *VGuiPaintFn)(void*, unsigned int);
static VGuiPaintFn o_VGuiPaint = NULL;
static void __fastcall hk_VGuiPaint(void* self, unsigned int mode)
{
    InterlockedIncrement(&g_vguipaint_calls);
    LONG n = g_hud_throttle;
    if (n > 1 && g_frame > 0 && (g_frame % n) != 0)
    {
        InterlockedIncrement(&g_vguipaint_skips);
        return;
    }
    o_VGuiPaint(self, mode);
}

typedef void (__fastcall *VGuiSimFn)(void*);
static VGuiSimFn o_VGuiSim = NULL;
static void __fastcall hk_VGuiSim(void* self)
{
    InterlockedIncrement(&g_vguisim_calls);
    LONG n = g_vguisim_throttle;
    if (n > 1 && g_frame > 0 && (g_frame % n) != 0)
    {
        InterlockedIncrement(&g_vguisim_skips);
        return;
    }
    o_VGuiSim(self);
}

// client.dll: shadow manager per-frame dirty-shadow processing
typedef void (__fastcall *ShadowPreFn)(void*);
static ShadowPreFn o_ShadowPre = NULL;
static void __fastcall hk_ShadowPre(void* self)
{
    InterlockedIncrement(&g_shadowpre_calls);
    if (!g_shadows_on)
    {
        InterlockedIncrement(&g_shadowpre_skips);
        return;
    }
    o_ShadowPre(self);
}

// ---------------------------------------------------------------------------
// distance culling (C_BaseEntity::GetRenderOrigin is vtable slot 9)
// ---------------------------------------------------------------------------
static float ent_dist_sq(void* self)
{
    if (!self) return 0.0f;
    typedef float* (__fastcall *GetOriginFn)(void*);
    GetOriginFn fn = (GetOriginFn)(*(void***)self)[9];
    if (!fn) return 0.0f;
    float* o = fn(self);
    if (!o) return 0.0f;
    float dx = o[0] - g_view_origin[0];
    float dy = o[1] - g_view_origin[1];
    float dz = o[2] - g_view_origin[2];
    return dx * dx + dy * dy + dz * dz;
}

static int far_cull(void* self)
{
    LONG d = g_far_dist;
    if (d <= 0) return 0;
    float f = (float)d;
    return ent_dist_sq(self) > f * f;
}

typedef int (__fastcall *BrushFn)(void*, unsigned char, int, char);
static BrushFn o_Brush = NULL;
static int __fastcall hk_Brush(void* self, unsigned char a2, int flags, char a4)
{
    g_brush_calls++;
    // never cull the shadow-depth/render-to-texture variants (bits 0x8000000 / 0x40000000)
    if (!(flags & (0x8000000 | 0x40000000)) && far_cull(self))
    {
        g_brush_skips++;
        return 1;
    }
    return o_Brush(self, a2, flags, a4);
}

typedef int (__fastcall *DrawModelFn)(void*, int);
static DrawModelFn o_Weapon = NULL;
static int __fastcall hk_Weapon(void* self, int flags)
{
    g_weapon_calls++;
    LONG d = g_far_weapons;
    if (d > 0)
    {
        float f = (float)d;
        if (ent_dist_sq(self) > f * f)
        {
            g_weapon_skips++;
            return 0;
        }
    }
    return o_Weapon(self, flags);
}

static DrawModelFn o_TempModel = NULL;
static int __fastcall hk_TempModel(void* self, int flags)
{
    g_tempmodel_calls++;
    if (far_cull(self))
    {
        g_tempmodel_skips++;
        return 0;
    }
    return o_TempModel(self, flags);
}

typedef bool (__fastcall *SetupBonesFn)(void*, void*, int, int, float);
static SetupBonesFn o_Bones = NULL;
static bool __fastcall hk_Bones(void* self, void* out, int nMaxBones, int boneMask, float t)
{
    // may run on shadow-pass worker threads -> keep interlocked counters
    InterlockedIncrement(&g_bones_calls);
    // only gate callers that want no output (shadow/attachment passes); never touch
    // the draw path, which needs the matrices
    LONG d = g_far_bones_dist;
    if (d > 0 && out == NULL)
    {
        float f = (float)d;
        if (ent_dist_sq(self) > f * f)
        {
            InterlockedIncrement(&g_bones_skips);
            return false;
        }
    }
    return o_Bones(self, out, nMaxBones, boneMask, t);
}

// ---------------------------------------------------------------------------
// menu detection: force stock behavior outside a match
// ---------------------------------------------------------------------------
static void* g_engine = NULL;
static int   g_engine_ok = 0;

static void engine_init(void)
{
    HMODULE e = GetModuleHandleA("engine.dll");
    if (!e) return;
    typedef void* (*CreateInterfaceFn)(const char*, int*);
    CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(e, "CreateInterface");
    if (!ci) return;

    // VEngineClient013 slot order is fixed by the SDK header; 014 is the fallback
    g_engine = ci("VEngineClient013", NULL);
    if (!g_engine) g_engine = ci("VEngineClient014", NULL);
    if (!g_engine) { logf_("engine: no client interface"); return; }

    // validate the vtable mapping before trusting it: GetMaxClients must look sane
    typedef int (__fastcall *GetMaxClientsFn)(void*);
    GetMaxClientsFn gmc = (GetMaxClientsFn)(*(void***)g_engine)[21];
    int mc = gmc ? gmc(g_engine) : 0;
    if (mc < 1 || mc > 100)
    {
        logf_("engine: GetMaxClients validation failed (%d), menu detection off", mc);
        g_engine = NULL;
        return;
    }
    g_engine_ok = 1;
    logf_("engine: client interface ok (maxclients=%d)", mc);
}

static int engine_in_game(void)
{
    if (!g_engine_ok) return 1;
    // require both signals to say "menu" before we suspend anything; any doubt = in game
    typedef int (__fastcall *IsInGameFn)(void*);
    IsInGameFn ig = (IsInGameFn)(*(void***)g_engine)[26];
    typedef int (__fastcall *GetLocalPlayerFn)(void*);
    GetLocalPlayerFn glp = (GetLocalPlayerFn)(*(void***)g_engine)[12];
    int a = ig ? ig(g_engine) : 1;
    int b = glp ? glp(g_engine) : 1;
    if (a != 0 || b > 0) return 1;
    return 0;
}

static void menu_apply(int in_game)
{
    if (!g_menuoff) return;
    if (!in_game && !g_menu_saved)
    {
        g_sv_mf = g_muzzleflash_cap; g_muzzleflash_cap = 0;
        g_sv_sim = g_sim_budget; g_sim_budget = 0;
        g_sv_anim = g_anim_dist; g_anim_dist = 0;
        g_sv_animth = g_anim_throttle; g_anim_throttle = 1;
        g_sv_far = g_far_dist; g_far_dist = 0;
        g_sv_farb = g_far_bones_dist; g_far_bones_dist = 0;
        g_sv_farw = g_far_weapons; g_far_weapons = 0;
        g_sv_shadows = g_shadows_on; g_shadows_on = 1;
        g_sv_vguisim = g_vguisim_throttle; g_vguisim_throttle = 1;
        g_sv_hud = g_hud_throttle; g_hud_throttle = 1;
        g_menu_saved = 1;
        logf_("menu: settings suspended (stock behavior)");
    }
    else if (in_game && g_menu_saved)
    {
        if (g_muzzleflash_cap == 0) g_muzzleflash_cap = g_sv_mf;
        if (g_sim_budget == 0) g_sim_budget = g_sv_sim;
        if (g_anim_dist == 0) g_anim_dist = g_sv_anim;
        if (g_anim_throttle == 1) g_anim_throttle = g_sv_animth;
        if (g_far_dist == 0) g_far_dist = g_sv_far;
        if (g_far_bones_dist == 0) g_far_bones_dist = g_sv_farb;
        if (g_far_weapons == 0) g_far_weapons = g_sv_farw;
        if (g_shadows_on == 1) g_shadows_on = g_sv_shadows;
        if (g_vguisim_throttle == 1) g_vguisim_throttle = g_sv_vguisim;
        if (g_hud_throttle == 1) g_hud_throttle = g_sv_hud;
        g_menu_saved = 0;
        logf_("menu: settings restored");
    }
}

// ---------------------------------------------------------------------------
// hook setup
// ---------------------------------------------------------------------------
static void setup_hooks(void)
{
    uint8_t* base = NULL; size_t size = 0;
    if (!get_module_range("client.dll", &base, &size))
    {
        logf_("FATAL: client.dll not loaded");
        return;
    }
    logf_("client.dll base=%p size=0x%08X", base, (unsigned)size);
    animlist_init(base, size);
    engine_init();

    // resolve + hook each target (per-module)
    for (int i = 0; i < (int)NUM_TARGETS; i++)
    {
        target_t* t = &g_targets[i];
        uint8_t* mb = NULL; size_t ms = 0;
        if (!get_module_range(t->module, &mb, &ms))
        {
            logf_("MISS  %-45s module %s not loaded", t->name, t->module);
            continue;
        }
        t->modbase = mb;
        pattern_t pat;
        uint8_t* hit = find_pattern(mb, ms, t->sig, &pat);
        if (!hit)
        {
            logf_("MISS  %-45s pattern not found in %s", t->name, t->module);
            continue;
        }
        t->addr = hit;
        logf_("FOUND %-45s %s+0x%08X", t->name, t->module, (unsigned)(hit - mb));
    }

    if (MH_Initialize() != MH_OK)
    {
        logf_("FATAL: MH_Initialize failed");
        return;
    }

    g_targets[0].detour = (void*)hk_RenderView;   g_targets[0].orig = (void**)&o_RenderView;
    g_targets[1].detour = (void*)hk_Muzzle;       g_targets[1].orig = (void**)&o_Muzzle;
    g_targets[2].detour = (void*)hk_MuzzleSentry; g_targets[2].orig = (void**)&o_MuzzleSentry;
    g_targets[3].detour = (void*)hk_Sim;          g_targets[3].orig = (void**)&o_Sim;
    g_targets[4].detour = (void*)hk_Anim;         g_targets[4].orig = (void**)&o_Anim;
    g_targets[5].detour = (void*)hk_Shadow;       g_targets[5].orig = (void**)&o_Shadow;
    g_targets[6].detour = (void*)hk_VGuiPaint;    g_targets[6].orig = (void**)&o_VGuiPaint;
    g_targets[7].detour = (void*)hk_VGuiSim;      g_targets[7].orig = (void**)&o_VGuiSim;
    g_targets[8].detour = (void*)hk_ShadowPre;    g_targets[8].orig = (void**)&o_ShadowPre;
    g_targets[9].detour = (void*)hk_Brush;        g_targets[9].orig = (void**)&o_Brush;
    g_targets[10].detour = (void*)hk_Weapon;      g_targets[10].orig = (void**)&o_Weapon;
    g_targets[11].detour = (void*)hk_TempModel;   g_targets[11].orig = (void**)&o_TempModel;
    g_targets[12].detour = (void*)hk_Bones;       g_targets[12].orig = (void**)&o_Bones;

    for (int i = 0; i < (int)NUM_TARGETS; i++)
    {
        target_t* t = &g_targets[i];
        if (!t->addr) continue;
        if (MH_CreateHook(t->addr, t->detour, t->orig) != MH_OK)
        {
            logf_("FAIL  %-45s MH_CreateHook", t->name);
            continue;
        }
        if (MH_EnableHook(t->addr) != MH_OK)
        {
            logf_("FAIL  %-45s MH_EnableHook", t->name);
            continue;
        }
        t->hooked = 1;
        logf_("HOOK  %-45s ok", t->name);
    }

    // cl_particle_retire_cost ConVar
    {
        pattern_t pat;
        uint8_t* hit = find_pattern(base, size, RETIRE_SIG, &pat);
        if (hit)
        {
            // instruction is `mov rdx, cs:[rip+disp32]`; disp is at hit+3, next insn at hit+7
            int32_t disp = *(int32_t*)(hit + 3);
            g_retire_cvar_parent_field = (void**)(hit + 7 + disp);
            logf_("FOUND cl_particle_retire_cost parent field at %p (rva=0x%08X)",
                  g_retire_cvar_parent_field, (unsigned)((uint8_t*)g_retire_cvar_parent_field - base));
        }
        else
        {
            logf_("MISS  cl_particle_retire_cost pattern not found");
        }
    }
}

static int retire_cvar_resolve(void)
{
    if (g_retire_cvar) return 1;
    if (!g_retire_cvar_parent_field) return 0;

    void* cv = *g_retire_cvar_parent_field;   // m_pParent (self)
    if (!cv) return 0;

    const char* name = *(const char**)((uint8_t*)cv + 0x18);
    if (!name || strcmp(name, "cl_particle_retire_cost") != 0)
    {
        logf_("retire: name validation failed (got '%s')", name ? name : "(null)");
        return 0;
    }
    void* parent = *(void**)((uint8_t*)cv + 0x38);
    if (parent != cv)
    {
        logf_("retire: parent self-check failed (%p vs %p), using parent", parent, cv);
        cv = parent;
    }
    g_retire_cvar = cv;
    logf_("retire: ConVar validated at %p", cv);
    return 1;
}

static int retire_cvar_set(float value)
{
    if (!retire_cvar_resolve()) return 0;
    *(float*)((uint8_t*)g_retire_cvar + 0x54) = value;
    *(int32_t*)((uint8_t*)g_retire_cvar + 0x58) = (int32_t)value;
    g_retire_cost = value;
    g_retire_cost_set = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// command handling (pipe thread)
// ---------------------------------------------------------------------------
static void append(char* out, size_t cap, const char* fmt, ...)
{
    size_t len = strlen(out);
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + len, cap - len - 1, fmt, ap);
    va_end(ap);
}

static const char* yn(LONG v) { return v ? "on" : "off"; }

static void cmd_help(char* out, size_t cap)
{
    append(out, cap,
        "tf2perf commands:\n"
        "  help                      this text\n"
        "  status                    hooks, addresses, counters, settings\n"
        "  retire <cost|off>         cl_particle_retire_cost (screen-area particle budget;\n"
        "                            off=0. try 0.5-2. culls heavy particle systems)\n"
        "  muzzleflash <n|off>       max 3rd-person muzzle flashes per frame (try 4-10)\n"
        "  simbudget <n|off>         max particle effects simulated per frame (try 32-128)\n"
        "  animthrottle <n|off>      run client-side animation updates every Nth frame.\n"
        "                            WARNING: twitches models and slows taunts - prefer animdist\n"
        "  animdist <dist|off>       skip animation updates only for entities beyond N units\n"
        "                            (keeps nearby players and taunts smooth; try 1500-3000)\n"
        "  shadows <on|off>          allow/skip depth-texture shadow pass AND shadow manager\n"
        "                            PreRender (dirty shadow/projected-texture updates)\n"
        "  hudthrottle <n|off>       paint the VGUI/HUD layer every Nth frame. WARNING: causes\n"
        "                            HUD flicker on many setups (surface not repainted) - leave off\n"
        "  vguisim <n|off>           run VGUI panel simulation every Nth frame (try 2)\n"
        "  far <dist|off>            distance-cull brush models and temp-ent models beyond N\n"
        "                            units (try 2000-4000; visual only)\n"
        "  farbones <dist|off>       skip no-output SetupBones calls beyond N units\n"
        "                            (shadow/attachment passes; experimental)\n"
        "  farweapons <dist|off>     cull weapon world models beyond N units. OFF by default:\n"
        "                            weapon origins are unreliable and can cull all weapons\n"
        "  menuoff <on|off>          suspend every setting outside a match (default on;\n"
        "                            needs IsInGame and GetLocalPlayer to agree)\n"
        "  alloff                    disable every setting at once (back to stock behavior)\n"
        "  reset                     zero all counters\n"
        "  quit                      close this session\n");
}

static void cmd_status(char* out, size_t cap)
{
    uint8_t* base = NULL; size_t size = 0;
    get_module_range("client.dll", &base, &size);

    append(out, cap, "tf2perf status\n");
    append(out, cap, "  frame=%ld  client.dll=%p (base rva shown per hook)\n", g_frame, base);

    for (int i = 0; i < (int)NUM_TARGETS; i++)
    {
        target_t* t = &g_targets[i];
        if (t->hooked)
            append(out, cap, "  [ok]   %-40s %s+0x%08X\n", t->name, t->module,
                   (unsigned)((uint8_t*)t->addr - t->modbase));
        else if (t->addr)
            append(out, cap, "  [--]   %-40s found but not hooked\n", t->name);
        else
            append(out, cap, "  [MISS] %-40s pattern not found\n", t->name);
    }

    if (retire_cvar_resolve())
        append(out, cap, "  [ok]   %-42s cvar=%p value=%.3f\n",
               "cl_particle_retire_cost", g_retire_cvar,
               *(float*)((uint8_t*)g_retire_cvar + 0x54));
    else
        append(out, cap, "  [MISS] %-42s\n", "cl_particle_retire_cost");

    append(out, cap,
        "settings:  retire=%s (%.3f)  muzzleflash=%s (%ld)  simbudget=%s (%ld)\n"
        "           animthrottle=%ld  animdist=%ld  shadows=%s  hudthrottle=%ld  vguisim=%ld\n"
        "           far=%ld  farbones=%ld  farweapons=%ld  menuoff=%s  in_game=%ld\n",
        g_retire_cost_set ? "set" : "default", g_retire_cost,
        g_muzzleflash_cap ? "on" : "off", g_muzzleflash_cap,
        g_sim_budget ? "on" : "off", g_sim_budget,
        g_anim_throttle, g_anim_dist, yn(g_shadows_on), g_hud_throttle, g_vguisim_throttle,
        g_far_dist, g_far_bones_dist, g_far_weapons, yn(g_menuoff), g_in_game);

    append(out, cap,
        "counters:  muzzleflash total=%ld skipped=%ld\n"
        "           sim total=%ld skipped=%ld\n"
        "           anim runs=%ld skips=%ld\n"
        "           shadow calls=%ld skipped=%ld\n"
        "           shadow pre calls=%ld skipped=%ld\n"
        "           vgui paint calls=%ld skipped=%ld\n"
        "           vgui sim   calls=%ld skipped=%ld\n"
        "           brush calls=%ld skipped=%ld\n"
        "           weapon calls=%ld skipped=%ld\n"
        "           tempmodel calls=%ld skipped=%ld\n"
        "           bones calls=%ld skipped=%ld\n",
        g_mf_total, g_mf_skipped,
        g_sim_total, g_sim_skipped,
        g_anim_runs, g_anim_skips,
        g_shadow_calls, g_shadow_skips,
        g_shadowpre_calls, g_shadowpre_skips,
        g_vguipaint_calls, g_vguipaint_skips,
        g_vguisim_calls, g_vguisim_skips,
        g_brush_calls, g_brush_skips,
        g_weapon_calls, g_weapon_skips,
        g_tempmodel_calls, g_tempmodel_skips,
        g_bones_calls, g_bones_skips);
}

static int parse_long(const char* s, long* out)
{
    char* end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != 0) return 0;
    *out = v;
    return 1;
}

static void cmd_execute(const char* line, char* out, size_t cap)
{
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    // trim
    char* s = buf;
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l-1] == '\n' || s[l-1] == '\r' || s[l-1] == ' ')) s[--l] = 0;

    char* arg = strchr(s, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }

    if (!*s) return;

    if (!_stricmp(s, "help") || !_stricmp(s, "?"))
    {
        cmd_help(out, cap);
    }
    else if (!_stricmp(s, "status"))
    {
        cmd_status(out, cap);
    }
    else if (!_stricmp(s, "retire"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off"))
        {
            g_retire_cost_set = 0;
            if (retire_cvar_set(0.0f))
                append(out, cap, "retire: off (cl_particle_retire_cost=0)\n");
            else
                append(out, cap, "retire: FAILED to resolve cl_particle_retire_cost\n");
        }
        else if (parse_long(arg, &v) && v >= 0 && v <= 100)
        {
            if (retire_cvar_set((float)v))
                append(out, cap, "retire: cl_particle_retire_cost=%.3f (budget %.0f px^2)\n",
                       (float)v, (float)v * 1000.0f);
            else
                append(out, cap, "retire: FAILED to resolve cl_particle_retire_cost\n");
        }
        else
        {
            append(out, cap, "usage: retire <0-100|off>\n");
        }
    }
    else if (!_stricmp(s, "muzzleflash"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_muzzleflash_cap = 0; append(out, cap, "muzzleflash: off\n"); }
        else if (parse_long(arg, &v) && v >= 1 && v <= 1000) { g_muzzleflash_cap = (LONG)v; append(out, cap, "muzzleflash: cap=%ld/frame\n", v); }
        else append(out, cap, "usage: muzzleflash <1-1000|off>\n");
    }
    else if (!_stricmp(s, "simbudget"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_sim_budget = 0; append(out, cap, "simbudget: off\n"); }
        else if (parse_long(arg, &v) && v >= 1 && v <= 100000) { g_sim_budget = (LONG)v; append(out, cap, "simbudget: %ld effects/frame\n", v); }
        else append(out, cap, "usage: simbudget <1-100000|off>\n");
    }
    else if (!_stricmp(s, "animdist"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_anim_dist = 0; append(out, cap, "animdist: off\n"); }
        else if (parse_long(arg, &v) && v >= 200 && v <= 20000)
        {
            g_anim_dist = (LONG)v;
            append(out, cap, "animdist: animation updates skipped beyond %ld units (near players stay smooth)\n", v);
        }
        else append(out, cap, "usage: animdist <200-20000|off>\n");
    }
    else if (!_stricmp(s, "animthrottle"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_anim_throttle = 1; append(out, cap, "animthrottle: off (every frame)\n"); }
        else if (parse_long(arg, &v) && v >= 1 && v <= 10) { g_anim_throttle = (LONG)v; append(out, cap, "animthrottle: every %ld frame(s)\n", v); }
        else append(out, cap, "usage: animthrottle <1-10|off>\n");
    }
    else if (!_stricmp(s, "shadows"))
    {
        if (!arg) { append(out, cap, "shadows: %s\n", yn(g_shadows_on)); }
        else if (!_stricmp(arg, "on")) { g_shadows_on = 1; append(out, cap, "shadows: on\n"); }
        else if (!_stricmp(arg, "off")) { g_shadows_on = 0; append(out, cap, "shadows: off (depth-texture pass skipped)\n"); }
        else append(out, cap, "usage: shadows <on|off>\n");
    }
    else if (!_stricmp(s, "hudthrottle"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_hud_throttle = 1; append(out, cap, "hudthrottle: off (HUD painted every frame)\n"); }
        else if (parse_long(arg, &v) && v >= 1 && v <= 10) { g_hud_throttle = (LONG)v; append(out, cap, "hudthrottle: HUD painted every %ld frame(s)\n", v); }
        else append(out, cap, "usage: hudthrottle <1-10|off>\n");
    }
    else if (!_stricmp(s, "vguisim"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_vguisim_throttle = 1; append(out, cap, "vguisim: off (every frame)\n"); }
        else if (parse_long(arg, &v) && v >= 1 && v <= 10) { g_vguisim_throttle = (LONG)v; append(out, cap, "vguisim: VGUI simulated every %ld frame(s)\n", v); }
        else append(out, cap, "usage: vguisim <1-10|off>\n");
    }
    else if (!_stricmp(s, "farweapons"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_far_weapons = 0; append(out, cap, "farweapons: off\n"); }
        else if (parse_long(arg, &v) && v >= 200 && v <= 20000)
        {
            g_far_weapons = (LONG)v;
            append(out, cap, "farweapons: culling weapon world models beyond %ld units (may hide weapons)\n", v);
        }
        else append(out, cap, "usage: farweapons <200-20000|off>\n");
    }
    else if (!_stricmp(s, "far"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_far_dist = 0; append(out, cap, "far: off\n"); }
        else if (parse_long(arg, &v) && v >= 200 && v <= 20000)
        {
            g_far_dist = (LONG)v;
            append(out, cap, "far: culling brush models / weapons / temp-ent models beyond %ld units\n", v);
        }
        else append(out, cap, "usage: far <200-20000|off>\n");
    }
    else if (!_stricmp(s, "farbones"))
    {
        long v;
        if (!arg || !_stricmp(arg, "off")) { g_far_bones_dist = 0; append(out, cap, "farbones: off\n"); }
        else if (parse_long(arg, &v) && v >= 200 && v <= 20000)
        {
            g_far_bones_dist = (LONG)v;
            append(out, cap, "farbones: skipping no-output SetupBones beyond %ld units\n", v);
        }
        else append(out, cap, "usage: farbones <200-20000|off>\n");
    }
    else if (!_stricmp(s, "menuoff"))
    {
        if (!arg) { append(out, cap, "menuoff: %s (in_game=%ld)\n", yn(g_menuoff), g_in_game); }
        else if (!_stricmp(arg, "on")) { g_menuoff = 1; append(out, cap, "menuoff: on (settings suspended outside a match)\n"); }
        else if (!_stricmp(arg, "off")) { g_menuoff = 0; append(out, cap, "menuoff: off\n"); }
        else append(out, cap, "usage: menuoff <on|off>\n");
    }
    else if (!_stricmp(s, "alloff") || !_stricmp(s, "off"))
    {
        g_muzzleflash_cap = 0;
        g_sim_budget = 0;
        g_anim_throttle = 1;
        g_shadows_on = 1;
        g_hud_throttle = 1;
        g_vguisim_throttle = 1;
        g_far_dist = 0;
        g_far_bones_dist = 0;
        g_far_weapons = 0;
        g_retire_cost_set = 0;
        retire_cvar_set(0.0f);
        g_retire_cost_set = 0;
        append(out, cap, "alloff: every hook disabled (stock behavior restored)\n");
    }
    else if (!_stricmp(s, "reset"))
    {
        g_mf_total = g_mf_skipped = 0;
        g_sim_total = g_sim_skipped = 0;
        g_anim_runs = g_anim_skips = 0;
        g_shadow_calls = g_shadow_skips = 0;
        append(out, cap, "counters reset\n");
    }
    else if (!_stricmp(s, "ping"))
    {
        append(out, cap, "pong\n");
    }
    else
    {
        append(out, cap, "unknown command '%s' (try help)\n", s);
    }
}

// ---------------------------------------------------------------------------
// pipe server
// ---------------------------------------------------------------------------
static DWORD WINAPI pipe_thread(LPVOID param)
{
    (void)param;
    char in[512];
    char out[8192];

    for (;;)
    {
        HANDLE h = CreateNamedPipeA(PIPE_NAME,
                                    PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES,
                                    8192, 8192, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
        {
            logf_("pipe: CreateNamedPipe failed err=%lu", GetLastError());
            Sleep(1000);
            continue;
        }

        if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(h);
            continue;
        }

        for (;;)
        {
            DWORD read = 0;
            memset(in, 0, sizeof(in));
            if (!ReadFile(h, in, sizeof(in) - 1, &read, NULL) || read == 0)
                break;

            memset(out, 0, sizeof(out));
            cmd_execute(in, out, sizeof(out));
            append(out, sizeof(out), "\n" END_MARK "\n");

            DWORD written = 0;
            WriteFile(h, out, (DWORD)strlen(out), &written, NULL);
        }

        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// init thread
// ---------------------------------------------------------------------------
static DWORD WINAPI init_thread(LPVOID param)
{
    (void)param;

    // wait for client.dll (injection can land before the renderer is up)
    for (int i = 0; i < 600 && !GetModuleHandleA("client.dll"); i++)
        Sleep(100);

    if (!GetModuleHandleA("client.dll"))
    {
        logf_("FATAL: timed out waiting for client.dll");
        return 0;
    }

    setup_hooks();
    logf_("tf2perf ready. pipe=%s", PIPE_NAME);
    return 0;
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
static HANDLE g_singleton = NULL;

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst; (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinst);

        g_singleton = CreateMutexA(NULL, FALSE, "tf2perf_singleton");
        if (g_singleton && GetLastError() == ERROR_ALREADY_EXISTS)
            return TRUE;   // already injected

        InitializeCriticalSection(&g_log_cs);
        g_log_ready = 1;

        {
            char path[MAX_PATH];
            DWORD n = GetTempPathA(sizeof(path), path);
            if (n && n < sizeof(path))
                strncat(path, "tf2perf.log", sizeof(path) - strlen(path) - 1);
            else
                strcpy(path, "tf2perf.log");
            g_log = fopen(path, "w");
        }

        logf_("tf2perf.dll attached");

        HANDLE t1 = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t1) CloseHandle(t1);
        HANDLE t2 = CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL);
        if (t2) CloseHandle(t2);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_log)
        {
            logf_("tf2perf.dll detaching");
            fclose(g_log);
            g_log = NULL;
        }
    }
    return TRUE;
}

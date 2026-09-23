// sigcheck.c - host-side verification of tf2perf patterns against the real DLLs
// Maps files raw (no loader), scans executable sections with the exact scanner
// tf2perf.dll uses, and compares each match RVA against the expected RVA from IDA.
//
// Build: build.cmd

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

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

static int count_pattern(uint8_t* base, size_t size, const char* sig, uint8_t** first, int max_hits)
{
    pattern_t pp;
    if (!parse_pattern(sig, &pp)) return -1;
    if ((size_t)pp.len > size) return 0;

    int hits = 0;
    *first = NULL;
    size_t limit = size - pp.len;
    for (size_t i = 0; i <= limit; i++)
    {
        if (pp.mask[0] && base[i] != pp.bytes[0]) continue;
        if (pp.mask[pp.len - 1] && base[i + pp.len - 1] != pp.bytes[pp.len - 1]) continue;
        int ok = 1;
        for (int j = 1; j < pp.len - 1; j++)
        {
            if (pp.mask[j] && base[i + j] != pp.bytes[j]) { ok = 0; break; }
        }
        if (ok)
        {
            if (!*first) *first = base + i;
            if (++hits >= max_hits) break;
        }
    }
    return hits;
}

typedef struct {
    const char* name;
    const char* module;
    const char* sig;
    unsigned    expected_rva;
} check_t;

static check_t g_checks[] = {
    { "CViewRender::RenderView", "client.dll",
      "48 8B C4 44 89 48 ? 44 89 40 ? 48 89 50 ? 48 89 48 ? 55 53",
      0x34B880 },
    { "TF_3rdPersonMuzzleFlashCallback", "client.dll",
      "4C 8B DC 48 81 EC C8 00 00 00 8B 41",
      0x50EA00 },
    { "TF_3rdPersonMuzzleFlash_SentryGun", "client.dll",
      "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 30 8B 71",
      0x50EEE0 },
    { "CParticleMgr::SimulateParticleEffect", "client.dll",
      "48 89 5C 24 ? 57 48 83 EC 20 48 8B 19 0F B6 79",
      0x2D82E0 },
    { "C_BaseAnimating::UpdateClientSideAnimations", "client.dll",
      "4C 8B DC 49 89 5B ? 49 89 6B ? 56 57 41 56 48 83 EC 60 48 8B 05 ? ? ? ? "
      "48 8D 1D ? ? ? ? 33 ED 48 8D 3D ? ? ? ? 49 89 6B ? 4C 8B 50 ? 4D 85 D2 74 ? "
      "49 89 5B ? 48 8D 05 ? ? ? ? 49 89 7B ? 49 8D 53 ? 49 89 43 ? 45 33 C9 48 8D 05 ? ? ? ? "
      "45 33 C0 49 89 43 ? 49 8B CA 48 8D 05 ? ? ? ? C7 44 24 ? ? ? ? ? 49 89 43 ? 49 89 6B ? "
      "41 FF 92 ? ? ? ? 48 8B 05 ? ? ? ? 48 8B AC 24 ? ? ? ? 48 8B 0D ? ? ? ? 48 8B 70 ? "
      "44 8B B1 ? ? ? ? 45 85 F6 74 ? C7 44 24 ? ? ? ? ? 4C 8B CF 45 33 C0 C6 44 24 ? ? "
      "48 8B D3 FF 15 ? ? ? ? 48 8B 0D",
      0x1D7570 },
    { "CClientShadowMgr::ComputeShadowDepthTextures", "client.dll",
      "48 8B C4 55 48 8D A8 ? ? ? ? 48 81 EC A0 09 00 00",
      0x248ED0 },
    { "cl_particle_retire_cost read site", "client.dll",
      "48 8B 15 ? ? ? ? 0F 57 C0 F3 0F 10 7A 54 F3 0F 59 3D",
      0x2D7437 },
    { "CEngineVGui::Paint", "engine.dll",
      "4C 8B DC 41 54 41 57 48 81 EC A8 00 00 00",
      0x226B00 },
    { "CEngineVGui::Simulate", "engine.dll",
      "41 57 48 81 EC A0 00 00 00 4C 8B F9",
      0x227700 },
    { "CClientShadowMgr::PreRender", "client.dll",
      "4C 8B DC 49 89 5B ? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 8B 05 ? ? ? ? 48 8D 35",
      0x24E000 },
    { "C_BaseEntity::DrawBrushModel", "client.dll",
      "4C 8B DC 49 89 5B ? 49 89 6B ? 49 89 73 ? 57 41 54 41 55 41 56 41 57 48 83 EC 70 48 8B 2D",
      0x1E19E0 },
    { "C_BaseCombatWeapon::DrawModel", "client.dll",
      "4C 8B DC 49 89 5B ? 49 89 6B ? 56 57 41 54 41 56 41 57 48 83 EC 60 48 8B 2D ? ? ? ? 48 8D 1D",
      0x1DC8C0 },
    { "C_LocalTempEntity::DrawStudioModel", "client.dll",
      "4C 8B DC 49 89 5B ? 89 54 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 70",
      0x471F10 },
    { "C_BaseAnimating::SetupBones", "client.dll",
      "48 8B C4 44 89 40 ? 48 89 50 ? 55 53",
      0x1D59A0 },
};

#define NUM_CHECKS (sizeof(g_checks) / sizeof(g_checks[0]))
#define MAX_SECS 32

typedef struct {
    const char* name;
    uint8_t*    file;
    size_t      size;
    uint8_t*    sec_base[MAX_SECS];
    size_t      sec_size[MAX_SECS];
    unsigned    sec_rva[MAX_SECS];
    int         sec_count;
    HANDLE      hfile;
    HANDLE      hmap;
} module_t;

static int map_module(module_t* mod, const char* name, const char* path)
{
    memset(mod, 0, sizeof(*mod));
    mod->name = name;

    mod->hfile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (mod->hfile == INVALID_HANDLE_VALUE)
    {
        printf("[-] CreateFile(%s) failed: %lu\n", path, GetLastError());
        return 0;
    }
    mod->hmap = CreateFileMappingA(mod->hfile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mod->hmap) { printf("[-] CreateFileMapping failed: %lu\n", GetLastError()); return 0; }
    mod->file = (uint8_t*)MapViewOfFile(mod->hmap, FILE_MAP_READ, 0, 0, 0);
    if (!mod->file) { printf("[-] MapViewOfFile failed: %lu\n", GetLastError()); return 0; }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod->file;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { printf("[-] %s: bad dos magic\n", name); return 0; }
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(mod->file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { printf("[-] %s: bad nt signature\n", name); return 0; }

    mod->size = nt->OptionalHeader.SizeOfImage;
    printf("[*] %s: x64=%d, %d sections, image size 0x%08X\n",
           name, nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC,
           nt->FileHeader.NumberOfSections, (unsigned)mod->size);

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && mod->sec_count < MAX_SECS; i++)
    {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && sec[i].SizeOfRawData)
        {
            mod->sec_base[mod->sec_count] = mod->file + sec[i].PointerToRawData;
            mod->sec_size[mod->sec_count] = sec[i].SizeOfRawData;
            mod->sec_rva[mod->sec_count]  = sec[i].VirtualAddress;
            printf("[*]   exec %-8.8s rva=0x%08X size=0x%08X\n",
                   sec[i].Name, sec[i].VirtualAddress, (unsigned)sec[i].SizeOfRawData);
            mod->sec_count++;
        }
    }
    return 1;
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL);
    const char* client_path = (argc >= 2) ? argv[1]
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Team Fortress 2\\tf\\bin\\x64\\client.dll";
    const char* engine_path = (argc >= 3) ? argv[2]
        : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Team Fortress 2\\bin\\x64\\engine.dll";

    static module_t mods[2];
    if (!map_module(&mods[0], "client.dll", client_path)) return 1;
    if (!map_module(&mods[1], "engine.dll", engine_path)) return 1;

    printf("\n%-46s %-8s %-10s %-10s %s\n", "check", "hits", "first rva", "expected", "result");
    printf("--------------------------------------------------------------------------------------\n");

    int fails = 0;
    for (int i = 0; i < (int)NUM_CHECKS; i++)
    {
        module_t* mod = NULL;
        for (int k = 0; k < 2; k++)
            if (!strcmp(mods[k].name, g_checks[i].module)) mod = &mods[k];

        int total_hits = 0;
        unsigned rva = 0;
        uint8_t* first_global = NULL;

        if (mod)
        {
            for (int s = 0; s < mod->sec_count; s++)
            {
                uint8_t* first = NULL;
                int hits = count_pattern(mod->sec_base[s], mod->sec_size[s], g_checks[i].sig, &first, 1000);
                if (hits > 0 && !first_global)
                {
                    first_global = first;
                    rva = mod->sec_rva[s] + (unsigned)(first - mod->sec_base[s]);
                }
                total_hits += hits;
            }
        }

        int pass = (total_hits >= 1 && rva == g_checks[i].expected_rva);
        if (!pass) fails++;
        printf("%-46s %-8d 0x%08X 0x%08X %s\n",
               g_checks[i].name, total_hits, rva, g_checks[i].expected_rva,
               pass ? "PASS" : (total_hits == 0 ? "NO MATCH" : "RVA MISMATCH"));
    }

    printf("--------------------------------------------------------------------------------------\n");
    printf("%d/%d passed\n", (int)NUM_CHECKS - fails, (int)NUM_CHECKS);

    for (int k = 0; k < 2; k++)
    {
        if (mods[k].file) UnmapViewOfFile(mods[k].file);
        if (mods[k].hmap) CloseHandle(mods[k].hmap);
        if (mods[k].hfile && mods[k].hfile != INVALID_HANDLE_VALUE) CloseHandle(mods[k].hfile);
    }
    return fails ? 1 : 0;
}

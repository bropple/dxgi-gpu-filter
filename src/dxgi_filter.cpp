// dxgi_filter.cpp
//
// A DXGI proxy DLL. Drop it next to a game's exe as "dxgi.dll" and it hides
// GPUs from the game by vendor ID (default: AMD, 0x1002), so the game can only
// enumerate the remaining adapter(s).
//
// How it works
//   * The game loads this dxgi.dll (application folder is searched first).
//   * It loads the real dxgi.dll from System32 and forwards every export to it.
//   * CreateDXGIFactory / CreateDXGIFactory1 / CreateDXGIFactory2 are wrapped:
//     after the real call, the factory's vtable is patched so that
//     EnumAdapters, EnumAdapters1, EnumAdapterByLuid and
//     EnumAdapterByGpuPreference skip adapters whose vendor ID is hidden.
//   * Safety: if hiding would remove EVERY adapter, nothing is hidden.
//
// Config (optional): dxgi_filter.ini next to this DLL
//   [Filter]
//   KeepNameContains=4080     ; KEEP MODE: only adapters whose name contains one
//   KeepVendorIds=10DE        ;   of these (or has one of these vendor IDs) stay
//                             ;   visible; everything else is hidden.
//   HideVendorIds=1002        ; HIDE MODE (used if no Keep* is set): hex, comma sep.
//   HideDuplicateWithoutOutputs=1
//   Log=1                     ; writes dxgi_filter.log next to the DLL
//
// Limits: it cannot affect code that never goes through DXGI enumeration, e.g.
// D3D12CreateDevice(nullptr, ...) or vendor libraries that talk to the driver
// directly.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Export tables
// ---------------------------------------------------------------------------
// Wrapped exports (implemented below).
#define WRAPPED(X) X(CreateDXGIFactory) X(CreateDXGIFactory1) X(CreateDXGIFactory2)

// Everything else is forwarded to the real dxgi.dll with a jmp thunk.
// Names that don't exist in the real DLL are harmless unless they are called.
#define PASSTHRU(X)                                                            \
    X(ApplyCompatResolutionQuirking) X(CompatString) X(CompatValue)            \
    X(D3DKMTCloseAdapter) X(D3DKMTDestroyAllocation) X(D3DKMTDestroyContext)   \
    X(D3DKMTDestroyDevice) X(D3DKMTDestroySynchronizationObject)               \
    X(D3DKMTQueryAdapterInfo) X(D3DKMTSetDisplayPrivateDriverFormat)           \
    X(D3DKMTSignalSynchronizationObject) X(D3DKMTUnlock)                       \
    X(D3DKMTWaitForSynchronizationObject)                                      \
    X(DXGID3D10CreateDevice) X(DXGID3D10CreateLayeredDevice)                   \
    X(DXGID3D10GetLayeredDeviceSize) X(DXGID3D10RegisterLayers)                \
    X(DXGIDeclareAdapterRemovalSupport) X(DXGIDumpJournal)                     \
    X(DXGIGetDebugInterface1) X(DXGIReportAdapterConfiguration)                \
    X(PIXBeginCapture) X(PIXEndCapture) X(PIXGetCaptureState)                  \
    X(SetAppCompatStringPointer) X(UpdateHMDEmulationStatus)

#define ALL_NAMES(X) WRAPPED(X) PASSTHRU(X)

// One function pointer per export, filled from the real dxgi.dll.
#define DECL_PTR(n) extern "C" { FARPROC real_##n __attribute__((used)) = nullptr; }
ALL_NAMES(DECL_PTR)

// Forwarding thunks: jmp through the pointer, preserving all arguments.
#define MAKE_THUNK(n)                                                          \
    __asm__(".text\n"                                                          \
            ".globl thunk_" #n "\n"                                            \
            "thunk_" #n ":\n\t"                                                \
            "jmp *real_" #n "(%rip)\n");
PASSTHRU(MAKE_THUNK)

// ---------------------------------------------------------------------------
// Config + logging
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_cs;
static std::vector<UINT> g_hideVendors;
static std::vector<UINT> g_keepVendors;
static std::vector<std::wstring> g_keepNames;   // lower-cased
static bool g_log = true;
static bool g_dedupe = false;
static char g_logPath[MAX_PATH * 3] = {};

static void Log(const char* fmt, ...)
{
    if (!g_log || !g_logPath[0]) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_cs);
    if (FILE* f = fopen(g_logPath, "ab")) {
        fprintf(f, "%s\r\n", buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

static std::string Narrow(const wchar_t* w)
{
    char out[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
}

static void LoadConfig(HMODULE self)
{
    wchar_t dll[MAX_PATH] = {};
    GetModuleFileNameW(self, dll, MAX_PATH);
    wchar_t dir[MAX_PATH] = {};
    wcsncpy(dir, dll, MAX_PATH - 1);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *(slash + 1) = 0;

    wchar_t ini[MAX_PATH * 2], logp[MAX_PATH * 2];
    swprintf(ini, MAX_PATH * 2, L"%ls%ls", dir, L"dxgi_filter.ini");
    swprintf(logp, MAX_PATH * 2, L"%ls%ls", dir, L"dxgi_filter.log");
    snprintf(g_logPath, sizeof(g_logPath), "%s", Narrow(logp).c_str());

    g_log = GetPrivateProfileIntW(L"Filter", L"Log", 1, ini) != 0;
    g_dedupe = GetPrivateProfileIntW(L"Filter", L"HideDuplicateWithoutOutputs", 0, ini) != 0;

    auto parseIds = [](const wchar_t* in, std::vector<UINT>& out) {
        wchar_t tmp[256] = {};
        wcsncpy(tmp, in, 255);
        for (wchar_t* p = tmp; *p;) {
            while (*p == L',' || *p == L' ' || *p == L';') ++p;
            if (!*p) break;
            wchar_t* end = nullptr;
            unsigned long v = wcstoul(p, &end, 16);
            if (end == p) break;
            out.push_back((UINT)v);
            p = end;
        }
    };

    wchar_t list[256] = {};
    GetPrivateProfileStringW(L"Filter", L"KeepVendorIds", L"", list, 256, ini);
    parseIds(list, g_keepVendors);

    wchar_t names[512] = {};
    GetPrivateProfileStringW(L"Filter", L"KeepNameContains", L"", names, 512, ini);
    for (wchar_t* p = names; *p;) {
        wchar_t* sep = wcschr(p, L',');
        std::wstring item(p, sep ? (size_t)(sep - p) : wcslen(p));
        // trim + lower-case
        while (!item.empty() && iswspace(item.front())) item.erase(item.begin());
        while (!item.empty() && iswspace(item.back())) item.pop_back();
        std::transform(item.begin(), item.end(), item.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
        if (!item.empty()) g_keepNames.push_back(item);
        if (!sep) break;
        p = sep + 1;
    }

    // Hide mode: default AMD only when no keep rule is given.
    GetPrivateProfileStringW(L"Filter", L"HideVendorIds",
                             (g_keepVendors.empty() && g_keepNames.empty()) ? L"1002" : L"", list, 256, ini);
    parseIds(list, g_hideVendors);
}

// ---------------------------------------------------------------------------
// Adapter filtering
// ---------------------------------------------------------------------------
static bool KeepModeActive() { return !g_keepVendors.empty() || !g_keepNames.empty(); }

static bool IsHidden(IDXGIAdapter* a)
{
    DXGI_ADAPTER_DESC d = {};
    if (FAILED(a->GetDesc(&d))) return false;

    if (KeepModeActive()) {
        for (UINT v : g_keepVendors)
            if (d.VendorId == v) return false;
        if (!g_keepNames.empty()) {
            std::wstring name(d.Description);
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
            for (const auto& k : g_keepNames)
                if (name.find(k) != std::wstring::npos) return false;
        }
        return true;   // not on the keep list
    }

    for (UINT v : g_hideVendors)
        if (d.VendorId == v) return true;
    return false;
}

static bool HasOutputs(IDXGIAdapter* a)
{
    IDXGIOutput* o = nullptr;
    if (SUCCEEDED(a->EnumOutputs(0, &o)) && o) { o->Release(); return true; }
    return false;
}

static void NoteAdapter(IDXGIAdapter* a)
{
    static std::set<std::pair<LONG, DWORD>> seen;
    DXGI_ADAPTER_DESC d = {};
    if (FAILED(a->GetDesc(&d))) return;
    EnterCriticalSection(&g_cs);
    bool fresh = seen.insert({d.AdapterLuid.HighPart, d.AdapterLuid.LowPart}).second;
    LeaveCriticalSection(&g_cs);
    if (fresh)
        Log("adapter: \"%s\" vendor=%04X device=%04X subsys=%08X vram=%llu MB outputs=%s luid=%08lX:%08lX -> %s",
            Narrow(d.Description).c_str(), d.VendorId, d.DeviceId, d.SubSysId,
            (unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)),
            HasOutputs(a) ? "yes" : "no",
            (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart,
            IsHidden(a) ? "hidden" : "kept");
}

// 'all' holds one reference per adapter (as returned by the real enumerator).
// Returns the i-th adapter after filtering, queried as 'riid'. Releases 'all'.
static HRESULT PickFiltered(std::vector<IDXGIAdapter*>& all, UINT i, REFIID riid, void** out)
{
    std::vector<IDXGIAdapter*> allowed;
    for (IDXGIAdapter* a : all) {
        NoteAdapter(a);
        if (!IsHidden(a)) allowed.push_back(a);
    }
    // Optional: drop an adapter that has no outputs when an identical
    // (same vendor+device) adapter with outputs exists. (Windows can list one
    // physical GPU twice.)
    if (g_dedupe && allowed.size() > 1) {
        std::vector<IDXGIAdapter*> kept;
        for (IDXGIAdapter* a : allowed) {
            DXGI_ADAPTER_DESC da = {};
            a->GetDesc(&da);
            bool drop = false;
            if (!HasOutputs(a)) {
                for (IDXGIAdapter* b : allowed) {
                    if (b == a) continue;
                    DXGI_ADAPTER_DESC db = {};
                    b->GetDesc(&db);
                    if (db.VendorId == da.VendorId && db.DeviceId == da.DeviceId && HasOutputs(b)) {
                        drop = true;
                        break;
                    }
                }
            }
            if (!drop) kept.push_back(a);
        }
        if (!kept.empty()) allowed.swap(kept);
    }

    // Never hide everything.
    const std::vector<IDXGIAdapter*>& use =
        (allowed.empty() || (allowed.size() == all.size())) ? all : allowed;

    HRESULT hr;
    if (i >= use.size()) {
        hr = DXGI_ERROR_NOT_FOUND;
    } else {
        hr = use[i]->QueryInterface(riid, out);
    }
    for (IDXGIAdapter* a : all) a->Release();
    return hr;
}

// Original (unpatched) vtable entries.
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumAdapters)(IDXGIFactory*, UINT, IDXGIAdapter**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumAdapters1)(IDXGIFactory1*, UINT, IDXGIAdapter1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumByLuid)(IDXGIFactory4*, LUID, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumByPref)(IDXGIFactory6*, UINT, DXGI_GPU_PREFERENCE, REFIID, void**);

static PFN_EnumAdapters  g_origEnum = nullptr;
static PFN_EnumAdapters1 g_origEnum1 = nullptr;
static PFN_EnumByLuid    g_origLuid = nullptr;
static PFN_EnumByPref    g_origPref = nullptr;

static HRESULT STDMETHODCALLTYPE Hook_EnumAdapters(IDXGIFactory* self, UINT i, IDXGIAdapter** out)
{
    if (!out) return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    std::vector<IDXGIAdapter*> all;
    for (UINT j = 0;; ++j) {
        IDXGIAdapter* a = nullptr;
        if (FAILED(g_origEnum(self, j, &a)) || !a) break;
        all.push_back(a);
    }
    return PickFiltered(all, i, __uuidof(IDXGIAdapter), (void**)out);
}

static HRESULT STDMETHODCALLTYPE Hook_EnumAdapters1(IDXGIFactory1* self, UINT i, IDXGIAdapter1** out)
{
    if (!out) return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    std::vector<IDXGIAdapter*> all;
    for (UINT j = 0;; ++j) {
        IDXGIAdapter1* a = nullptr;
        if (FAILED(g_origEnum1(self, j, &a)) || !a) break;
        all.push_back(a);
    }
    return PickFiltered(all, i, __uuidof(IDXGIAdapter1), (void**)out);
}

static HRESULT STDMETHODCALLTYPE Hook_EnumAdapterByGpuPreference(
    IDXGIFactory6* self, UINT i, DXGI_GPU_PREFERENCE pref, REFIID riid, void** out)
{
    if (!out) return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    std::vector<IDXGIAdapter*> all;
    for (UINT j = 0;; ++j) {
        IDXGIAdapter* a = nullptr;
        if (FAILED(g_origPref(self, j, pref, __uuidof(IDXGIAdapter), (void**)&a)) || !a) break;
        all.push_back(a);
    }
    return PickFiltered(all, i, riid, out);
}

static HRESULT STDMETHODCALLTYPE Hook_EnumAdapterByLuid(
    IDXGIFactory4* self, LUID luid, REFIID riid, void** out)
{
    HRESULT hr = g_origLuid(self, luid, riid, out);
    if (FAILED(hr) || !out || !*out) return hr;

    IUnknown* u = (IUnknown*)*out;
    IDXGIAdapter* a = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(IDXGIAdapter), (void**)&a)) && a) {
        NoteAdapter(a);
        bool hide = IsHidden(a);
        a->Release();
        if (hide) {
            // Only hide if some other adapter remains: ask the (hooked)
            // EnumAdapters1, which already applies the "never hide all" rule.
            bool otherExists = false;
            IDXGIFactory1* f1 = nullptr;
            if (SUCCEEDED(self->QueryInterface(__uuidof(IDXGIFactory1), (void**)&f1)) && f1) {
                IDXGIAdapter1* first = nullptr;
                if (SUCCEEDED(f1->EnumAdapters1(0, &first)) && first) {
                    otherExists = !IsHidden(first);
                    first->Release();
                }
                f1->Release();
            }
            if (otherExists) {
                u->Release();
                *out = nullptr;
                return DXGI_ERROR_NOT_FOUND;
            }
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// vtable patching
// ---------------------------------------------------------------------------
static void PatchSlot(IUnknown* obj, REFIID iid, int slot, void* hook, void** orig)
{
    void* p = nullptr;
    if (FAILED(obj->QueryInterface(iid, &p)) || !p) return;

    void** vtbl = *(void***)p;
    EnterCriticalSection(&g_cs);
    if (vtbl[slot] != hook) {
        if (!*orig) *orig = vtbl[slot];
        if (*orig == vtbl[slot]) {
            DWORD old = 0;
            if (VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) {
                vtbl[slot] = hook;
                VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
                Log("patched vtable slot %d", slot);
            }
        } else {
            Log("slot %d has a different original; left alone", slot);
        }
    }
    LeaveCriticalSection(&g_cs);
    ((IUnknown*)p)->Release();
}

static void PatchFactory(IUnknown* f)
{
    // Slot numbers are the stable COM vtable positions in the IDXGIFactory chain.
    PatchSlot(f, __uuidof(IDXGIFactory),  7,  (void*)Hook_EnumAdapters,                (void**)&g_origEnum);
    PatchSlot(f, __uuidof(IDXGIFactory1), 12, (void*)Hook_EnumAdapters1,               (void**)&g_origEnum1);
    PatchSlot(f, __uuidof(IDXGIFactory4), 26, (void*)Hook_EnumAdapterByLuid,           (void**)&g_origLuid);
    PatchSlot(f, __uuidof(IDXGIFactory6), 29, (void*)Hook_EnumAdapterByGpuPreference,  (void**)&g_origPref);
}

// ---------------------------------------------------------------------------
// Wrapped exports
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* PFN_CDF)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CDF2)(UINT, REFIID, void**);

extern "C" HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void** pp)
{
    if (!real_CreateDXGIFactory) return E_FAIL;
    HRESULT hr = ((PFN_CDF)(void*)real_CreateDXGIFactory)(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) PatchFactory((IUnknown*)*pp);
    return hr;
}

extern "C" HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** pp)
{
    if (!real_CreateDXGIFactory1) return E_FAIL;
    HRESULT hr = ((PFN_CDF)(void*)real_CreateDXGIFactory1)(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) PatchFactory((IUnknown*)*pp);
    return hr;
}

extern "C" HRESULT WINAPI Hook_CreateDXGIFactory2(UINT flags, REFIID riid, void** pp)
{
    if (!real_CreateDXGIFactory2) return E_FAIL;
    HRESULT hr = ((PFN_CDF2)(void*)real_CreateDXGIFactory2)(flags, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) PatchFactory((IUnknown*)*pp);
    return hr;
}

// ---------------------------------------------------------------------------
// DLL entry
// ---------------------------------------------------------------------------
extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        LoadConfig(inst);

        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        wcsncat(sys, L"\\dxgi.dll", MAX_PATH - wcslen(sys) - 1);
        HMODULE real = LoadLibraryW(sys);
        if (!real) return FALSE;

#define RESOLVE(n) real_##n = GetProcAddress(real, #n);
        ALL_NAMES(RESOLVE)

        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Log("---- dxgi_filter loaded in %s", Narrow(exe).c_str());
        if (KeepModeActive()) {
            std::string ids, nm;
            for (UINT v : g_keepVendors) { char t[16]; snprintf(t, sizeof(t), "%04X ", v); ids += t; }
            for (const auto& k : g_keepNames) { nm += Narrow(k.c_str()); nm += " | "; }
            Log("KEEP mode. vendor ids: %s ; names: %s", ids.empty() ? "(none)" : ids.c_str(),
                nm.empty() ? "(none)" : nm.c_str());
        } else {
            std::string ids;
            for (UINT v : g_hideVendors) { char t[16]; snprintf(t, sizeof(t), "%04X ", v); ids += t; }
            Log("HIDE mode. vendor ids: %s", ids.empty() ? "(none)" : ids.c_str());
        }
    }
    return TRUE;
}

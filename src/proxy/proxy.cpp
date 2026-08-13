// Bully DX12 Wrapper — M1: d3d9.dll forwarding proxy with device-creation logging.
//
// Dropped next to Bully.exe, this DLL is resolved by the game's dynamic
// LoadLibrary("D3D9.DLL") and by d3dx9_38's static import. It forwards every
// d3d9.dll export to the real system d3d9.dll, and wraps the IDirect3D9
// returned by Direct3DCreate9 to log device creation.
//
// M2 will redirect Direct3DCreate9 to Direct3DCreate9On12 instead.
//
// Build: x86 only.  cmake -B build/proxy -A Win32 src/proxy
//        cmake --build build/proxy --config Release

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>

// IID_IDirect3D9 defined locally (we are BUILDING d3d9.dll, so we cannot
// import the GUID from d3d9.lib): {81BDCBCA-64D4-426D-AE8D-AD0147F4275C}
static const GUID kIID_IDirect3D9 = {
    0x81bdcbca, 0x64d4, 0x426d, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}};

// ---------------------------------------------------------------------------
// Logging: append to bully_d3d9proxy.log next to the exe.
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_log) {
        g_log = _fsopen("bully_d3d9proxy.log", "a", _SH_DENYNO);
    }
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// Real system d3d9.dll (loaded by absolute path to avoid recursion).
// ---------------------------------------------------------------------------
static HMODULE g_realD3D9 = nullptr;

// Forwarded export function pointers.
static FARPROC g_Direct3DCreate9            = nullptr;
static FARPROC g_Direct3DCreate9Ex          = nullptr;
static FARPROC g_Direct3DShaderValidatorCreate9 = nullptr;
static FARPROC g_PSGPError                  = nullptr;
static FARPROC g_PSGPSampleTexture          = nullptr;
static FARPROC g_D3DPERF_BeginEvent         = nullptr;
static FARPROC g_D3DPERF_EndEvent           = nullptr;
static FARPROC g_D3DPERF_GetStatus          = nullptr;
static FARPROC g_D3DPERF_QueryRepeatFrame   = nullptr;
static FARPROC g_D3DPERF_SetMarker          = nullptr;
static FARPROC g_D3DPERF_SetOptions          = nullptr;
static FARPROC g_D3DPERF_SetRegion          = nullptr;
static FARPROC g_DebugSetLevel              = nullptr;
static FARPROC g_DebugSetMute               = nullptr;
static FARPROC g_Direct3DCreate9On12        = nullptr;
static FARPROC g_Direct3DCreate9On12Ex      = nullptr;

static void LoadRealD3D9() {
    if (g_realD3D9) return;
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\d3d9.dll");
    g_realD3D9 = LoadLibraryA(sysPath);
    Log("[proxy] LoadLibrary real d3d9: %s -> 0x%p\n", sysPath, g_realD3D9);
    if (!g_realD3D9) return;

    g_Direct3DCreate9 = GetProcAddress(g_realD3D9, "Direct3DCreate9");
    g_Direct3DCreate9Ex = GetProcAddress(g_realD3D9, "Direct3DCreate9Ex");
    g_Direct3DShaderValidatorCreate9 = GetProcAddress(g_realD3D9, "Direct3DShaderValidatorCreate9");
    g_PSGPError = GetProcAddress(g_realD3D9, "PSGPError");
    g_PSGPSampleTexture = GetProcAddress(g_realD3D9, "PSGPSampleTexture");
    g_D3DPERF_BeginEvent = GetProcAddress(g_realD3D9, "D3DPERF_BeginEvent");
    g_D3DPERF_EndEvent = GetProcAddress(g_realD3D9, "D3DPERF_EndEvent");
    g_D3DPERF_GetStatus = GetProcAddress(g_realD3D9, "D3DPERF_GetStatus");
    g_D3DPERF_QueryRepeatFrame = GetProcAddress(g_realD3D9, "D3DPERF_QueryRepeatFrame");
    g_D3DPERF_SetMarker = GetProcAddress(g_realD3D9, "D3DPERF_SetMarker");
    g_D3DPERF_SetOptions = GetProcAddress(g_realD3D9, "D3DPERF_SetOptions");
    g_D3DPERF_SetRegion = GetProcAddress(g_realD3D9, "D3DPERF_SetRegion");
    g_DebugSetLevel = GetProcAddress(g_realD3D9, "DebugSetLevel");
    g_DebugSetMute = GetProcAddress(g_realD3D9, "DebugSetMute");
    g_Direct3DCreate9On12 = GetProcAddress(g_realD3D9, "Direct3DCreate9On12");
    g_Direct3DCreate9On12Ex = GetProcAddress(g_realD3D9, "Direct3DCreate9On12Ex");
}

// ---------------------------------------------------------------------------
// Trampolines: naked jmp through the pointer variable. Stack is preserved
// verbatim, so any calling convention and any signature works.
// ---------------------------------------------------------------------------
#define TRAMPOLINE(name)                                                        \
    extern "C" __declspec(naked) void WINAPI proxy_##name() {                   \
        __asm { jmp dword ptr [g_##name] }                                      \
    }

TRAMPOLINE(Direct3DCreate9Ex)
TRAMPOLINE(Direct3DShaderValidatorCreate9)
TRAMPOLINE(PSGPError)
TRAMPOLINE(PSGPSampleTexture)
TRAMPOLINE(D3DPERF_BeginEvent)
TRAMPOLINE(D3DPERF_EndEvent)
TRAMPOLINE(D3DPERF_GetStatus)
TRAMPOLINE(D3DPERF_QueryRepeatFrame)
TRAMPOLINE(D3DPERF_SetMarker)
TRAMPOLINE(D3DPERF_SetOptions)
TRAMPOLINE(D3DPERF_SetRegion)
TRAMPOLINE(DebugSetLevel)
TRAMPOLINE(DebugSetMute)
TRAMPOLINE(Direct3DCreate9On12)
TRAMPOLINE(Direct3DCreate9On12Ex)

// ---------------------------------------------------------------------------
// IDirect3D9 wrapper: logs device creation, forwards everything else.
// Only methods the game can call are affected; vtable order is fixed across
// all d3d9 SDK versions (17 methods), so this is ABI-safe.
// ---------------------------------------------------------------------------
class ProxyIDirect3D9 : public IDirect3D9 {
public:
    ProxyIDirect3D9(IDirect3D9* inner) : m_inner(inner), m_refs(1) {
        Log("[d3d9] IDirect3D9 wrapped (0x%p), adapters=%u\n",
            inner, inner ? inner->GetAdapterCount() : 0);
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) {
        if (ppvObj == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIID_IDirect3D9) {
            AddRef();
            *ppvObj = static_cast<IDirect3D9*>(this);
            return S_OK;
        }
        return m_inner->QueryInterface(riid, ppvObj);
    }
    STDMETHOD_(ULONG, AddRef)() { return ++m_refs; }
    STDMETHOD_(ULONG, Release)() {
        ULONG r = --m_refs;
        if (r == 0) {
            m_inner->Release();
            delete this;
        }
        return r;
    }

    // IDirect3D9
    STDMETHOD(RegisterSoftwareDevice)(void* pInitializeFunction) {
        return m_inner->RegisterSoftwareDevice(pInitializeFunction);
    }
    STDMETHOD_(UINT, GetAdapterCount)() { return m_inner->GetAdapterCount(); }
    STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD Flags,
                                    D3DADAPTER_IDENTIFIER9* pIdentifier) {
        return m_inner->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
    }
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter, D3DFORMAT Format) {
        return m_inner->GetAdapterModeCount(Adapter, Format);
    }
    STDMETHOD(EnumAdapterModes)(UINT Adapter, D3DFORMAT Format, UINT Mode,
                                D3DDISPLAYMODE* pMode) {
        return m_inner->EnumAdapterModes(Adapter, Format, Mode, pMode);
    }
    STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE* pMode) {
        return m_inner->GetAdapterDisplayMode(Adapter, pMode);
    }
    STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE DevType,
                               D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat,
                               BOOL bWindowed) {
        return m_inner->CheckDeviceType(Adapter, DevType, AdapterFormat,
                                        BackBufferFormat, bWindowed);
    }
    STDMETHOD(CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE DeviceType,
                                 D3DFORMAT AdapterFormat, DWORD Usage,
                                 D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
        return m_inner->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat,
                                          Usage, RType, CheckFormat);
    }
    STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE DeviceType,
                                          D3DFORMAT SurfaceFormat, BOOL Windowed,
                                          D3DMULTISAMPLE_TYPE MultiSampleType,
                                          DWORD* pQualityLevels) {
        return m_inner->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat,
                                                   Windowed, MultiSampleType, pQualityLevels);
    }
    STDMETHOD(CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE DeviceType,
                                      D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
                                      D3DFORMAT DepthStencilFormat) {
        return m_inner->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat,
                                               RenderTargetFormat, DepthStencilFormat);
    }
    STDMETHOD(CheckDeviceFormatConversion)(UINT Adapter, D3DDEVTYPE DeviceType,
                                           D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
        return m_inner->CheckDeviceFormatConversion(Adapter, DeviceType,
                                                    SourceFormat, TargetFormat);
    }
    STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) {
        return m_inner->GetDeviceCaps(Adapter, DeviceType, pCaps);
    }
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) {
        return m_inner->GetAdapterMonitor(Adapter);
    }
    STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE DeviceType,
                            HWND hFocusWindow, DWORD BehaviorFlags,
                            D3DPRESENT_PARAMETERS* pPresentationParameters,
                            IDirect3DDevice9** ppReturnedDeviceInterface) {
        if (pPresentationParameters) {
            Log("[d3d9] CreateDevice(Adapter=%u, DevType=%u, Flags=0x%x, "
                "bb=%ux%u fmt=%u, fullscreen=%d, vsync=%u, msaa=%u/%u, depthfmt=%u)\n",
                Adapter, DeviceType, BehaviorFlags,
                pPresentationParameters->BackBufferWidth,
                pPresentationParameters->BackBufferHeight,
                pPresentationParameters->BackBufferFormat,
                pPresentationParameters->Windowed ? 0 : 1,
                pPresentationParameters->PresentationInterval,
                pPresentationParameters->MultiSampleType,
                pPresentationParameters->MultiSampleQuality,
                pPresentationParameters->AutoDepthStencilFormat);
        } else {
            Log("[d3d9] CreateDevice(Adapter=%u, DevType=%u, Flags=0x%x, PP=NULL)\n",
                Adapter, DeviceType, BehaviorFlags);
        }
        HRESULT hr = m_inner->CreateDevice(Adapter, DeviceType, hFocusWindow,
                                           BehaviorFlags, pPresentationParameters,
                                           ppReturnedDeviceInterface);
        Log("  -> hr=0x%08x, device=0x%p\n", hr,
            ppReturnedDeviceInterface ? *ppReturnedDeviceInterface : nullptr);
        return hr;
    }

private:
    IDirect3D9* m_inner;
    ULONG m_refs;
};

// ---------------------------------------------------------------------------
// Direct3DCreate9 hook (M1: forward + wrap; M2: redirect to 9On12).
// ---------------------------------------------------------------------------
extern "C" IDirect3D9* WINAPI proxy_Direct3DCreate9(UINT SDKVersion) {
    Log("[d3d9] Direct3DCreate9(SDKVersion=%u)\n", SDKVersion);
    LoadRealD3D9();
    if (!g_Direct3DCreate9) return nullptr;
    auto real = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(g_Direct3DCreate9);
    IDirect3D9* d = real(SDKVersion);
    if (d) {
        return new ProxyIDirect3D9(d);
    }
    Log("  -> real Direct3DCreate9 returned NULL\n");
    return nullptr;
}

// ---------------------------------------------------------------------------
BOOLEAN WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    (void)hinstDLL;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            Log("----------------------------------------------\n");
            Log("[proxy] d3d9 proxy attached (pid=%lu)\n", GetCurrentProcessId());
            LoadRealD3D9();
            break;
        case DLL_PROCESS_DETACH:
            Log("[proxy] d3d9 proxy detached\n");
            break;
        default:
            break;
    }
    return TRUE;
}

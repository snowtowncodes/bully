// Bully DX12 Wrapper — M2: d3d9.dll forwarding proxy with optional 9On12 backend.
//
// Dropped next to Bully.exe, this DLL is resolved by the game's dynamic
// LoadLibrary("D3D9.DLL") and by d3dx9_38's static import. It forwards every
// d3d9.dll export to the real system d3d9.dll, and wraps the IDirect3D9
// returned by Direct3DCreate9 to log device creation.
//
// Build: x86 only.  cmake -B build/proxy -A Win32 src/proxy
//        cmake --build build/proxy --config Release

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <new>
#include <share.h>
#include <cmath>

// IID_IDirect3D9 defined locally (we are BUILDING d3d9.dll, so we cannot
// import the GUID from d3d9.lib): {81BDCBCA-64D4-426D-AE8D-AD0147F4275C}
static const GUID kIID_IDirect3D9 = {
    0x81bdcbca, 0x64d4, 0x426d, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}};

// IID_IDirect3DDevice9 {D0223B96-BF7A-43FD-92BD-A43B0D82B9EB}
static const GUID kIID_IDirect3DDevice9 = {
    0xd0223b96, 0xbf7a, 0x43fd, {0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb}};

enum class RendererBackend {
    On12,
    Native,
    Dxvk,
};

static const char* BackendName(RendererBackend backend) {
    switch (backend) {
    case RendererBackend::On12:
        return "on12";
    case RendererBackend::Dxvk:
        return "dxvk";
    case RendererBackend::Native:
        return "native";
    }
    return "native";
}

enum class On12DeviceMode {
    Internal,
    Explicit,
};

enum class D3D12DebugLayerMode {
    None,
    Registry,
};

static const char* D3D12DebugLayerModeName(D3D12DebugLayerMode mode) {
    return mode == D3D12DebugLayerMode::Registry ? "registry" : "none";
}

// ---------------------------------------------------------------------------
// Thread-safe logging: append to bully_d3d9proxy.log next to the exe.
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static INIT_ONCE g_logInitOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_logCS;

static BOOL CALLBACK InitLogOnce(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_logCS);
    g_log = _fsopen("bully_d3d9proxy.log", "a", _SH_DENYNO);
    return TRUE;
}

static void Log(const char* fmt, ...) {
    InitOnceExecuteOnce(&g_logInitOnce, InitLogOnce, nullptr, nullptr);
    if (!g_log) return;

    EnterCriticalSection(&g_logCS);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    LeaveCriticalSection(&g_logCS);
}

// ---------------------------------------------------------------------------
// Real system d3d9.dll (loaded by absolute path to avoid recursion).
// Thread-safe lazy initialization using INIT_ONCE.
// ---------------------------------------------------------------------------
static HMODULE g_realD3D9 = nullptr;
static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

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

static BOOL CALLBACK InitRealD3D9Once(PINIT_ONCE, PVOID, PVOID*) {
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\d3d9.dll");
    g_realD3D9 = LoadLibraryA(sysPath);
    Log("[proxy] LoadLibrary real d3d9: %s -> 0x%p\n", sysPath, g_realD3D9);
    if (!g_realD3D9) return TRUE;

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
    return TRUE;
}

static void EnsureRealD3D9Loaded() {
    InitOnceExecuteOnce(&g_initOnce, InitRealD3D9Once, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Renderer backend selection. The INI lives beside the game executable.
// ---------------------------------------------------------------------------
static bool GetExeSiblingPath(const char* filename, char* path, size_t pathSize) {
    DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(pathSize));
    if (length == 0 || length >= pathSize) {
        strcpy_s(path, pathSize, ".\\");
        strcat_s(path, pathSize, filename);
        return false;
    }

    char* separator = strrchr(path, '\\');
    if (!separator) separator = strrchr(path, '/');
    if (separator) {
        *(separator + 1) = '\0';
    } else {
        path[0] = '\0';
    }
    strcat_s(path, pathSize, filename);
    return true;
}

// DXVK is loaded only from beside the executable and intentionally remains
// loaded until process exit so returned COM objects retain valid code pointers.
static HMODULE g_dxvkD3D9 = nullptr;
static FARPROC g_DxvkDirect3DCreate9 = nullptr;
static char g_dxvkD3D9Path[MAX_PATH] = {};
static INIT_ONCE g_dxvkD3D9InitOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitDxvkD3D9Once(PINIT_ONCE, PVOID, PVOID*) {
    const bool resolved = GetExeSiblingPath(
        "dxvk_d3d9.dll", g_dxvkD3D9Path, ARRAYSIZE(g_dxvkD3D9Path));
    if (!resolved) {
        Log("[dxvk] could not resolve executable path for module; load skipped (path=%s)\n",
            g_dxvkD3D9Path);
        return TRUE;
    }

    g_dxvkD3D9 = LoadLibraryA(g_dxvkD3D9Path);
    Log("[dxvk] LoadLibrary: %s -> 0x%p\n", g_dxvkD3D9Path, g_dxvkD3D9);
    if (!g_dxvkD3D9) return TRUE;

    g_DxvkDirect3DCreate9 = GetProcAddress(g_dxvkD3D9, "Direct3DCreate9");
    Log("[dxvk] GetProcAddress(Direct3DCreate9): %s -> 0x%p\n",
        g_dxvkD3D9Path, g_DxvkDirect3DCreate9);
    return TRUE;
}

static void EnsureDxvkD3D9Loaded() {
    InitOnceExecuteOnce(&g_dxvkD3D9InitOnce, InitDxvkD3D9Once, nullptr, nullptr);
}

static bool GetRendererIniPath(char* path, size_t pathSize) {
    bool resolved = GetExeSiblingPath("bully_d3d9proxy.ini", path, pathSize);
    if (!resolved) {
        Log("[proxy] could not resolve executable path for renderer INI; using %s\n", path);
    }
    return resolved;
}

static RendererBackend ReadRendererBackend() {
    char iniPath[MAX_PATH] = {};
    GetRendererIniPath(iniPath, ARRAYSIZE(iniPath));

    char value[32] = {};
    GetPrivateProfileStringA("renderer", "backend", "native",
                             value, ARRAYSIZE(value), iniPath);

    if (_stricmp(value, "native") == 0) {
        Log("[proxy] requested backend=native (ini=%s)\n", iniPath);
        return RendererBackend::Native;
    }
    if (_stricmp(value, "on12") == 0) {
        Log("[proxy] requested backend=on12 (ini=%s)\n", iniPath);
        return RendererBackend::On12;
    }
    if (_stricmp(value, "dxvk") == 0) {
        Log("[proxy] requested backend=dxvk (ini=%s)\n", iniPath);
        return RendererBackend::Dxvk;
    }

    Log("[proxy] requested backend=%s is unrecognized; using native default (ini=%s)\n",
        value, iniPath);
    return RendererBackend::Native;
}

static On12DeviceMode ReadOn12DeviceMode() {
    char iniPath[MAX_PATH] = {};
    GetRendererIniPath(iniPath, ARRAYSIZE(iniPath));

    char value[32] = {};
    GetPrivateProfileStringA("renderer", "on12_device", "internal",
                             value, ARRAYSIZE(value), iniPath);

    if (value[0] == '\0' || _stricmp(value, "internal") == 0) {
        Log("[proxy] requested on12_device=internal (ini=%s)\n", iniPath);
        return On12DeviceMode::Internal;
    }
    if (_stricmp(value, "explicit") == 0) {
        Log("[proxy] requested on12_device=explicit (ini=%s)\n", iniPath);
        return On12DeviceMode::Explicit;
    }

    Log("[proxy] requested on12_device=%s is unrecognized; using internal default (ini=%s)\n",
        value, iniPath);
    return On12DeviceMode::Internal;
}

struct DeviceDiagnosticsConfig {
    bool traceDevice;
    bool captureFrames;
    bool captureFrontBuffer;
    UINT captureFrame;
    bool testMarker;
    UINT testMarkerSize;
    D3D12DebugLayerMode d3d12DebugLayer;

    // Presentation parameter overrides (empty/none = no override)
    char forceSwapEffect[16];      // "none", "discard", "flip", "copy"
    char forcePresentInterval[16]; // "none", "immediate", "default", "one"
};;

static DeviceDiagnosticsConfig ReadDeviceDiagnosticsConfig() {
    char iniPath[MAX_PATH] = {};
    GetRendererIniPath(iniPath, ARRAYSIZE(iniPath));

    DeviceDiagnosticsConfig config = {};
    config.traceDevice = GetPrivateProfileIntA(
        "diagnostics", "trace_device", 1, iniPath) != 0;
    config.captureFrames = GetPrivateProfileIntA(
        "diagnostics", "capture_frames", 1, iniPath) != 0;
    config.captureFrontBuffer = GetPrivateProfileIntA(
        "diagnostics", "capture_frontbuffer", 0, iniPath) != 0;
    config.captureFrame = GetPrivateProfileIntA(
        "diagnostics", "capture_frame", 60, iniPath);
    config.testMarker = GetPrivateProfileIntA(
        "mods", "test_marker", 0, iniPath) != 0;
    config.testMarkerSize = GetPrivateProfileIntA(
        "mods", "test_marker_size", 64, iniPath);
    if (config.testMarkerSize < 8) config.testMarkerSize = 8;
    if (config.testMarkerSize > 512) config.testMarkerSize = 512;

    char d3d12DebugLayer[32] = {};
    GetPrivateProfileStringA("diagnostics", "d3d12_debug_layer", "none",
                             d3d12DebugLayer, ARRAYSIZE(d3d12DebugLayer), iniPath);
    if (_stricmp(d3d12DebugLayer, "registry") == 0) {
        config.d3d12DebugLayer = D3D12DebugLayerMode::Registry;
    } else if (d3d12DebugLayer[0] != '\0' && _stricmp(d3d12DebugLayer, "none") != 0) {
        Log("[proxy] invalid d3d12_debug_layer=%s; using none (valid: none/registry)\n",
            d3d12DebugLayer);
    }

    // Read presentation parameter overrides
    GetPrivateProfileStringA("renderer", "force_swap_effect", "none",
                            config.forceSwapEffect, ARRAYSIZE(config.forceSwapEffect), iniPath);
    GetPrivateProfileStringA("renderer", "force_present_interval", "none",
                            config.forcePresentInterval, ARRAYSIZE(config.forcePresentInterval), iniPath);

    Log("[proxy] diagnostics trace_device=%u, capture_frames=%u, capture_frontbuffer=%u, "
        "capture_frame=%u, d3d12_debug_layer=%s\n",
        config.traceDevice ? 1u : 0u, config.captureFrames ? 1u : 0u,
        config.captureFrontBuffer ? 1u : 0u, config.captureFrame,
        D3D12DebugLayerModeName(config.d3d12DebugLayer));
    if (config.d3d12DebugLayer == D3D12DebugLayerMode::Registry) {
        Log("[proxy] diagnostics d3d12_debug_layer=registry: proxy does not modify the "
            "registry or enable a D3D12 debug layer; set the active HKLM D3D9On12 "
            "UseDebugLayer control externally before device creation.\n");
    }
    Log("[proxy] presentation overrides force_swap_effect=%s, force_present_interval=%s\n",
        config.forceSwapEffect, config.forcePresentInterval);
    Log("[proxy] mod test_marker=%u, test_marker_size=%u\n",
        config.testMarker ? 1u : 0u, config.testMarkerSize);
    return config;
}

// Apply presentation parameter overrides based on config
static bool ApplyPresentationOverrides(const DeviceDiagnosticsConfig& config,
                                       const D3DPRESENT_PARAMETERS* pSource,
                                       D3DPRESENT_PARAMETERS* pDest) {
    if (!pSource || !pDest) return false;

    // Copy source parameters
    *pDest = *pSource;

    bool anyOverride = false;
    DWORD originalSwapEffect = pSource->SwapEffect;
    UINT originalPresentInterval = pSource->PresentationInterval;

    // Apply swap effect override
    if (_stricmp(config.forceSwapEffect, "none") != 0) {
        if (_stricmp(config.forceSwapEffect, "discard") == 0) {
            pDest->SwapEffect = D3DSWAPEFFECT_DISCARD;
            anyOverride = true;
        } else if (_stricmp(config.forceSwapEffect, "flip") == 0) {
            pDest->SwapEffect = D3DSWAPEFFECT_FLIP;
            anyOverride = true;
        } else if (_stricmp(config.forceSwapEffect, "copy") == 0) {
            pDest->SwapEffect = D3DSWAPEFFECT_COPY;
            anyOverride = true;
        } else {
            Log("[proxy] invalid force_swap_effect='%s', ignoring (valid: none/discard/flip/copy)\n",
                config.forceSwapEffect);
        }
    }

    // Apply present interval override
    if (_stricmp(config.forcePresentInterval, "none") != 0) {
        if (_stricmp(config.forcePresentInterval, "immediate") == 0) {
            pDest->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            anyOverride = true;
        } else if (_stricmp(config.forcePresentInterval, "default") == 0) {
            pDest->PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
            anyOverride = true;
        } else if (_stricmp(config.forcePresentInterval, "one") == 0) {
            pDest->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            anyOverride = true;
        } else {
            Log("[proxy] invalid force_present_interval='%s', ignoring (valid: none/immediate/default/one)\n",
                config.forcePresentInterval);
        }
    }

    if (anyOverride) {
        Log("[proxy] presentation parameter overrides applied:\n");
        Log("  SwapEffect: %u -> %u\n", originalSwapEffect, pDest->SwapEffect);
        Log("  PresentationInterval: 0x%08x -> 0x%08x\n", originalPresentInterval, pDest->PresentationInterval);
    }

    return anyOverride;
}

template <typename T>
static void ReleaseCom(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

static const char* D3D12DeviceRemovedReasonLabel(HRESULT result) {
    switch (result) {
    case S_OK:
        return "S_OK";
    case DXGI_ERROR_DEVICE_HUNG:
        return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED:
        return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:
        return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:
        return "DXGI_ERROR_INVALID_CALL";
    case E_OUTOFMEMORY:
        return "E_OUTOFMEMORY";
    case E_FAIL:
        return "E_FAIL";
    default:
        return "unrecognized";
    }
}

static void LogD3D12DeviceRemovedReason(ID3D12Device* d3d12Device, const char* phase) {
    if (!d3d12Device) {
        Log("[d3d12] device removal reason (%s): unavailable (ID3D12Device is NULL)\n", phase);
        return;
    }

    const HRESULT result = d3d12Device->GetDeviceRemovedReason();
    Log("[d3d12] device removal reason (%s): hr=0x%08lx, result=%s, diagnostic_only=1\n",
        phase, static_cast<unsigned long>(result), D3D12DeviceRemovedReasonLabel(result));
}

static void LogVerifiedOn12RuntimeIdentity(ID3D12Device* d3d12Device) {
    char d3d9RuntimePath[MAX_PATH] = {};
    const DWORD pathLength = g_realD3D9
        ? GetModuleFileNameA(g_realD3D9, d3d9RuntimePath, ARRAYSIZE(d3d9RuntimePath))
        : 0;
    const char* runtimePath = pathLength > 0 && pathLength < ARRAYSIZE(d3d9RuntimePath)
        ? d3d9RuntimePath
        : "<unavailable>";
    Log("[d3d12] deployed runtime identity: effective_backend=on12, d3d9_runtime=\"%s\", "
        "d3d9on12_interface=verified, d3d9on12_runtime=not-observable, "
        "d3d12_device=0x%p, "
        "actual_d3d12_feature_level=not-observable\n",
        runtimePath, d3d12Device);
}

// Holds the proxy's own references. The context is transferred only after
// Direct3DCreate9On12 succeeds and is released after that inner enumerator.
struct ExplicitOn12Context {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    ExplicitOn12Context() = default;
    ExplicitOn12Context(const ExplicitOn12Context&) = delete;
    ExplicitOn12Context& operator=(const ExplicitOn12Context&) = delete;

    ~ExplicitOn12Context() {
        Reset();
    }

    void Reset() {
        ReleaseCom(queue);
        ReleaseCom(device);
    }
};

static HMONITOR GetNativeD3D9AdapterMonitor(UINT SDKVersion) {
    if (!g_Direct3DCreate9) {
        Log("[d3d9] explicit on12 adapter mapping: real Direct3DCreate9 export unavailable\n");
        return nullptr;
    }

    auto realDirect3DCreate9 = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(g_Direct3DCreate9);
    IDirect3D9* nativeEnumerator = realDirect3DCreate9(SDKVersion);
    if (!nativeEnumerator) {
        Log("[d3d9] explicit on12 adapter mapping: temporary real Direct3DCreate9 returned NULL\n");
        return nullptr;
    }

    const UINT adapterCount = nativeEnumerator->GetAdapterCount();
    HMONITOR monitor = adapterCount > 0 ? nativeEnumerator->GetAdapterMonitor(0) : nullptr;
    Log("[d3d9] explicit on12 adapter mapping: temporary real D3D9 enumerator=0x%p, "
        "adapters=%u, adapter0_monitor=0x%p\n",
        static_cast<void*>(nativeEnumerator), adapterCount, static_cast<void*>(monitor));
    nativeEnumerator->Release();
    return monitor;
}

static bool AdapterHasOutputMonitor(IDXGIAdapter1* adapter, UINT adapterIndex,
                                    HMONITOR targetMonitor) {
    if (!adapter || !targetMonitor) return false;

    for (UINT outputIndex = 0;; ++outputIndex) {
        IDXGIOutput* output = nullptr;
        HRESULT outputHr = adapter->EnumOutputs(outputIndex, &output);
        if (outputHr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(outputHr)) {
            Log("[d3d9] explicit on12 adapter mapping: EnumOutputs(adapter=%u, output=%u) "
                "failed hr=0x%08lx\n",
                adapterIndex, outputIndex, static_cast<unsigned long>(outputHr));
            ReleaseCom(output);
            break;
        }
        if (!output) {
            Log("[d3d9] explicit on12 adapter mapping: EnumOutputs(adapter=%u, output=%u) "
                "succeeded with a NULL output\n",
                adapterIndex, outputIndex);
            break;
        }

        DXGI_OUTPUT_DESC outputDesc = {};
        HRESULT descHr = output->GetDesc(&outputDesc);
        ReleaseCom(output);
        if (FAILED(descHr)) {
            Log("[d3d9] explicit on12 adapter mapping: GetDesc(adapter=%u, output=%u) "
                "failed hr=0x%08lx\n",
                adapterIndex, outputIndex, static_cast<unsigned long>(descHr));
            continue;
        }

        if (outputDesc.Monitor == targetMonitor) {
            Log("[d3d9] explicit on12 adapter mapping: monitor match at "
                "dxgi_adapter=%u, output=%u\n",
                adapterIndex, outputIndex);
            return true;
        }
    }
    return false;
}

static bool CreateExplicitOn12Context(UINT SDKVersion, ExplicitOn12Context* context) {
    if (!context) return false;
    context->Reset();

    const HMONITOR d3d9AdapterMonitor = GetNativeD3D9AdapterMonitor(SDKVersion);
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapterAtIndexZero = nullptr;
    IDXGIAdapter1* firstHardwareAdapter = nullptr;
    IDXGIAdapter1* monitorMatchedHardwareAdapter = nullptr;
    IDXGIAdapter1* monitorMatchedSoftwareAdapter = nullptr;
    IDXGIAdapter1* selectedAdapter = nullptr;
    UINT firstHardwareAdapterIndex = 0;
    UINT monitorMatchedHardwareAdapterIndex = 0;
    UINT monitorMatchedSoftwareAdapterIndex = 0;
    UINT selectedAdapterIndex = 0;
    bool adapterAtIndexZeroIsSoftware = false;
    const char* mappingMethod = nullptr;
    bool succeeded = false;

    do {
        HRESULT factoryHr = CreateDXGIFactory1(
            __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(factoryHr)) {
            Log("[d3d9] explicit on12 setup failed: CreateDXGIFactory1 hr=0x%08lx\n",
                static_cast<unsigned long>(factoryHr));
            break;
        }

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            IDXGIAdapter1* candidate = nullptr;
            HRESULT enumHr = factory->EnumAdapters1(adapterIndex, &candidate);
            if (enumHr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumHr)) {
                Log("[d3d9] explicit on12 adapter mapping: EnumAdapters1(index=%u) "
                    "failed hr=0x%08lx\n",
                    adapterIndex, static_cast<unsigned long>(enumHr));
                ReleaseCom(candidate);
                break;
            }
            if (!candidate) {
                Log("[d3d9] explicit on12 adapter mapping: EnumAdapters1(index=%u) "
                    "succeeded with a NULL adapter\n",
                    adapterIndex);
                break;
            }

            DXGI_ADAPTER_DESC1 candidateDesc = {};
            HRESULT descHr = candidate->GetDesc1(&candidateDesc);
            if (FAILED(descHr)) {
                Log("[d3d9] explicit on12 adapter mapping: GetDesc1(index=%u) "
                    "failed hr=0x%08lx\n",
                    adapterIndex, static_cast<unsigned long>(descHr));
                ReleaseCom(candidate);
                continue;
            }

            const bool isSoftware =
                (candidateDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            if (adapterIndex == 0) {
                adapterAtIndexZero = candidate;
                adapterAtIndexZero->AddRef();
                adapterAtIndexZeroIsSoftware = isSoftware;
            }
            if (!isSoftware && !firstHardwareAdapter) {
                firstHardwareAdapter = candidate;
                firstHardwareAdapter->AddRef();
                firstHardwareAdapterIndex = adapterIndex;
            }

            if (AdapterHasOutputMonitor(candidate, adapterIndex, d3d9AdapterMonitor)) {
                if (isSoftware) {
                    if (!monitorMatchedSoftwareAdapter) {
                        monitorMatchedSoftwareAdapter = candidate;
                        monitorMatchedSoftwareAdapter->AddRef();
                        monitorMatchedSoftwareAdapterIndex = adapterIndex;
                    }
                } else if (!monitorMatchedHardwareAdapter) {
                    monitorMatchedHardwareAdapter = candidate;
                    monitorMatchedHardwareAdapter->AddRef();
                    monitorMatchedHardwareAdapterIndex = adapterIndex;
                }
            }
            ReleaseCom(candidate);
        }

        if (!adapterAtIndexZero) {
            Log("[d3d9] explicit on12 setup failed: DXGI adapter index 0 is unavailable\n");
            break;
        }

        if (d3d9AdapterMonitor && monitorMatchedHardwareAdapter) {
            selectedAdapter = monitorMatchedHardwareAdapter;
            selectedAdapter->AddRef();
            selectedAdapterIndex = monitorMatchedHardwareAdapterIndex;
            mappingMethod = "d3d9-monitor-to-dxgi-output";
        } else if (d3d9AdapterMonitor && monitorMatchedSoftwareAdapter && !firstHardwareAdapter) {
            selectedAdapter = monitorMatchedSoftwareAdapter;
            selectedAdapter->AddRef();
            selectedAdapterIndex = monitorMatchedSoftwareAdapterIndex;
            mappingMethod = "d3d9-monitor-to-dxgi-output-software-only";
        } else {
            if (!d3d9AdapterMonitor) {
                Log("[d3d9] explicit on12 adapter mapping: D3D9 adapter 0 monitor unavailable; "
                    "using DXGI fallback\n");
            } else if (monitorMatchedSoftwareAdapter && firstHardwareAdapter) {
                Log("[d3d9] explicit on12 adapter mapping: matched DXGI adapter %u is software; "
                    "rejecting it because hardware adapter %u is available\n",
                    monitorMatchedSoftwareAdapterIndex, firstHardwareAdapterIndex);
            } else {
                Log("[d3d9] explicit on12 adapter mapping: no DXGI output matched D3D9 adapter 0 "
                    "monitor; using DXGI fallback\n");
            }

            if (adapterAtIndexZeroIsSoftware && firstHardwareAdapter) {
                selectedAdapter = firstHardwareAdapter;
                selectedAdapter->AddRef();
                selectedAdapterIndex = firstHardwareAdapterIndex;
                mappingMethod = "dxgi-index-0-software-rejected-first-hardware";
                Log("[d3d9] explicit on12 adapter mapping: DXGI adapter index 0 is software; "
                    "using first hardware adapter index %u instead\n",
                    firstHardwareAdapterIndex);
            } else {
                selectedAdapter = adapterAtIndexZero;
                selectedAdapter->AddRef();
                selectedAdapterIndex = 0;
                mappingMethod = "dxgi-adapter-index-0-fallback";
                Log("[d3d9] explicit on12 adapter mapping: using DXGI adapter index 0 fallback "
                    "(WARP was not requested)\n");
            }
        }

        DXGI_ADAPTER_DESC1 selectedDesc = {};
        HRESULT selectedDescHr = selectedAdapter->GetDesc1(&selectedDesc);
        if (FAILED(selectedDescHr)) {
            Log("[d3d9] explicit on12 setup failed: GetDesc1(selected index=%u) "
                "hr=0x%08lx\n",
                selectedAdapterIndex, static_cast<unsigned long>(selectedDescHr));
            break;
        }
        selectedDesc.Description[ARRAYSIZE(selectedDesc.Description) - 1] = L'\0';
        Log("[d3d9] explicit on12 adapter selected: mapping=%s, dxgi_index=%u, "
            "d3d9_adapter0_monitor=0x%p, description=\"%ls\", VendorId=0x%04lx, "
            "DeviceId=0x%04lx, LUID=%08lx:%08lx\n",
            mappingMethod, selectedAdapterIndex, static_cast<void*>(d3d9AdapterMonitor),
            selectedDesc.Description, static_cast<unsigned long>(selectedDesc.VendorId),
            static_cast<unsigned long>(selectedDesc.DeviceId),
            static_cast<unsigned long>(static_cast<DWORD>(selectedDesc.AdapterLuid.HighPart)),
            static_cast<unsigned long>(selectedDesc.AdapterLuid.LowPart));

        HRESULT deviceHr = D3D12CreateDevice(
            selectedAdapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&context->device));
        if (FAILED(deviceHr) || !context->device) {
            Log("[d3d9] explicit on12 setup failed: D3D12CreateDevice(adapter=%u, "
                "feature_level=11_0) hr=0x%08lx, device=0x%p\n",
                selectedAdapterIndex, static_cast<unsigned long>(deviceHr),
                static_cast<void*>(context->device));
            break;
        }
        Log("[d3d9] explicit on12 D3D12 device created: device=0x%p, node_count=%u, "
            "requested_feature_level=11_0\n",
            static_cast<void*>(context->device), context->device->GetNodeCount());

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;
        HRESULT queueHr = context->device->CreateCommandQueue(
            &queueDesc, __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void**>(&context->queue));
        if (FAILED(queueHr) || !context->queue) {
            Log("[d3d9] explicit on12 setup failed: CreateCommandQueue(type=DIRECT) "
                "hr=0x%08lx, queue=0x%p\n",
                static_cast<unsigned long>(queueHr), static_cast<void*>(context->queue));
            break;
        }
        Log("[d3d9] explicit on12 command queue created: queue=0x%p, "
            "queue_type=D3D12_COMMAND_LIST_TYPE_DIRECT\n",
            static_cast<void*>(context->queue));
        succeeded = true;
    } while (false);

    ReleaseCom(selectedAdapter);
    ReleaseCom(monitorMatchedSoftwareAdapter);
    ReleaseCom(monitorMatchedHardwareAdapter);
    ReleaseCom(firstHardwareAdapter);
    ReleaseCom(adapterAtIndexZero);
    ReleaseCom(factory);

    if (!succeeded) {
        if (context->device || context->queue) {
            Log("[d3d9] explicit on12 setup failed: releasing partial D3D12 context\n");
        }
        context->Reset();
    }
    return succeeded;
}

static void LogExplicitDeviceIdentityMatch(ID3D12Device* returnedDevice,
                                           ID3D12Device* suppliedDevice) {
    if (!suppliedDevice) return;

    IUnknown* returnedIdentity = nullptr;
    IUnknown* suppliedIdentity = nullptr;
    HRESULT returnedIdentityHr = returnedDevice
        ? returnedDevice->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&returnedIdentity))
        : E_NOINTERFACE;
    HRESULT suppliedIdentityHr = suppliedDevice->QueryInterface(
        IID_IUnknown, reinterpret_cast<void**>(&suppliedIdentity));
    const bool matchesExplicit = SUCCEEDED(returnedIdentityHr) &&
        SUCCEEDED(suppliedIdentityHr) && returnedIdentity == suppliedIdentity;
    Log("[d3d9] device backend probe: explicit D3D12 COM identity "
        "returned_iunknown_hr=0x%08lx, supplied_iunknown_hr=0x%08lx, "
        "matches_explicit=%u\n",
        static_cast<unsigned long>(returnedIdentityHr),
        static_cast<unsigned long>(suppliedIdentityHr), matchesExplicit ? 1u : 0u);
    ReleaseCom(returnedIdentity);
    ReleaseCom(suppliedIdentity);
}

static bool VerifyDeviceBackend(IDirect3DDevice9* device, RendererBackend backend,
                                ID3D12Device* suppliedExplicitDevice) {
    if (backend == RendererBackend::Dxvk) {
        Log("[d3d9] device backend probe skipped: effective=dxvk\n");
        return false;
    }

    if (!device) {
        Log("[d3d9] device backend: native/unverified (device pointer is NULL)\n");
        LogExplicitDeviceIdentityMatch(nullptr, suppliedExplicitDevice);
        return false;
    }

    IDirect3DDevice9On12* deviceOn12 = nullptr;
    HRESULT queryHr = device->QueryInterface(
        __uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&deviceOn12));
    Log("[d3d9] device backend probe (effective=%s): "
        "QueryInterface(IDirect3DDevice9On12) hr=0x%08lx, iface=0x%p\n",
        BackendName(backend), static_cast<unsigned long>(queryHr), deviceOn12);

    if (SUCCEEDED(queryHr) && deviceOn12) {
        ID3D12Device* d3d12Device = nullptr;
        HRESULT deviceHr = deviceOn12->GetD3D12Device(
            __uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12Device));
        Log("[d3d9] device backend probe: GetD3D12Device(ID3D12Device) "
            "hr=0x%08lx, device=0x%p\n",
            static_cast<unsigned long>(deviceHr), d3d12Device);

        if (SUCCEEDED(deviceHr) && d3d12Device) {
            Log("[d3d9] device backend verified: D3D12\n");
            if (backend == RendererBackend::On12) {
                LogVerifiedOn12RuntimeIdentity(d3d12Device);
                LogD3D12DeviceRemovedReason(d3d12Device, "post-verify");
            }
        } else {
            Log("[d3d9] device backend: native/unverified "
                "(D3D12 device query failed)\n");
        }
        LogExplicitDeviceIdentityMatch(d3d12Device, suppliedExplicitDevice);
        const bool verifiedOn12Device =
            backend == RendererBackend::On12 && SUCCEEDED(deviceHr) && d3d12Device;
        if (d3d12Device) d3d12Device->Release();
        deviceOn12->Release();
        return verifiedOn12Device;
    }

    if (deviceOn12) deviceOn12->Release();
    Log("[d3d9] device backend: native/unverified\n");
    LogExplicitDeviceIdentityMatch(nullptr, suppliedExplicitDevice);
    return false;
}

static void LogFinalD3D12DeviceRemovedReason(IDirect3DDevice9* device) {
    if (!device) {
        Log("[d3d12] device removal reason (device-wrapper-final-release): unavailable "
            "(IDirect3DDevice9 is NULL)\n");
        return;
    }

    IDirect3DDevice9On12* deviceOn12 = nullptr;
    const HRESULT on12Hr = device->QueryInterface(
        __uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&deviceOn12));
    if (FAILED(on12Hr) || !deviceOn12) {
        Log("[d3d12] device removal reason (device-wrapper-final-release): unavailable; "
            "QueryInterface(IDirect3DDevice9On12) hr=0x%08lx, iface=0x%p\n",
            static_cast<unsigned long>(on12Hr), deviceOn12);
        ReleaseCom(deviceOn12);
        return;
    }

    ID3D12Device* d3d12Device = nullptr;
    const HRESULT d3d12Hr = deviceOn12->GetD3D12Device(
        __uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12Device));
    if (SUCCEEDED(d3d12Hr) && d3d12Device) {
        LogD3D12DeviceRemovedReason(d3d12Device, "device-wrapper-final-release");
    } else {
        Log("[d3d12] device removal reason (device-wrapper-final-release): unavailable; "
            "GetD3D12Device(ID3D12Device) hr=0x%08lx, device=0x%p\n",
            static_cast<unsigned long>(d3d12Hr), d3d12Device);
    }
    ReleaseCom(d3d12Device);
    ReleaseCom(deviceOn12);
}

// ---------------------------------------------------------------------------
// Fail-fast handler for missing exports (non-returning)
// ---------------------------------------------------------------------------
__declspec(noreturn) static void FailFastMissingExport(const char* name) {
    Log("[proxy] FATAL: export %s unavailable in system d3d9.dll\n", name);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

// ---------------------------------------------------------------------------
// Forwarding exports: exact ABI for documented exports, safe trampolines for
// undocumented/private exports that preserve all registers and stack.
// ---------------------------------------------------------------------------

// Direct3DCreate9Ex: HRESULT WINAPI Direct3DCreate9Ex(UINT, IDirect3D9Ex**)
extern "C" HRESULT WINAPI proxy_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D9Ex) {
    EnsureRealD3D9Loaded();
    if (!g_Direct3DCreate9Ex) return E_NOTIMPL;
    if (!ppD3D9Ex) return E_POINTER;
    *ppD3D9Ex = nullptr;
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>(g_Direct3DCreate9Ex);
    return fn(SDKVersion, ppD3D9Ex);
}

// Direct3DCreate9On12: IDirect3D9* WINAPI Direct3DCreate9On12(UINT, D3D9ON12_ARGS*, UINT)
extern "C" IDirect3D9* WINAPI proxy_Direct3DCreate9On12(UINT SDKVersion, D3D9ON12_ARGS* pArgs, UINT NumArgs) {
    EnsureRealD3D9Loaded();
    if (!g_Direct3DCreate9On12) return nullptr;
    auto fn = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT)>(g_Direct3DCreate9On12);
    return fn(SDKVersion, pArgs, NumArgs);
}

// Direct3DCreate9On12Ex: HRESULT WINAPI Direct3DCreate9On12Ex(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**)
extern "C" HRESULT WINAPI proxy_Direct3DCreate9On12Ex(UINT SDKVersion, D3D9ON12_ARGS* pArgs, UINT NumArgs, IDirect3D9Ex** ppD3D9Ex) {
    EnsureRealD3D9Loaded();
    if (!g_Direct3DCreate9On12Ex) return E_NOTIMPL;
    if (!ppD3D9Ex) return E_POINTER;
    *ppD3D9Ex = nullptr;
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**)>(g_Direct3DCreate9On12Ex);
    return fn(SDKVersion, pArgs, NumArgs, ppD3D9Ex);
}

// D3DPERF_* exports: documented, known signatures
extern "C" int WINAPI proxy_D3DPERF_BeginEvent(DWORD col, LPCWSTR wszName) {
    EnsureRealD3D9Loaded();
    if (!g_D3DPERF_BeginEvent) return -1;
    auto fn = reinterpret_cast<int(WINAPI*)(DWORD, LPCWSTR)>(g_D3DPERF_BeginEvent);
    return fn(col, wszName);
}

extern "C" int WINAPI proxy_D3DPERF_EndEvent() {
    EnsureRealD3D9Loaded();
    if (!g_D3DPERF_EndEvent) return -1;
    auto fn = reinterpret_cast<int(WINAPI*)()>(g_D3DPERF_EndEvent);
    return fn();
}

extern "C" DWORD WINAPI proxy_D3DPERF_GetStatus() {
    EnsureRealD3D9Loaded();
    if (!g_D3DPERF_GetStatus) return 0;
    auto fn = reinterpret_cast<DWORD(WINAPI*)()>(g_D3DPERF_GetStatus);
    return fn();
}

extern "C" BOOL WINAPI proxy_D3DPERF_QueryRepeatFrame() {
    EnsureRealD3D9Loaded();
    if (!g_D3DPERF_QueryRepeatFrame) return FALSE;
    auto fn = reinterpret_cast<BOOL(WINAPI*)()>(g_D3DPERF_QueryRepeatFrame);
    return fn();
}

extern "C" void WINAPI proxy_D3DPERF_SetMarker(DWORD col, LPCWSTR wszName) {
    EnsureRealD3D9Loaded();
    if (!g_D3DPERF_SetMarker) return;
    auto fn = reinterpret_cast<void(WINAPI*)(DWORD, LPCWSTR)>(g_D3DPERF_SetMarker);
    fn(col, wszName);
}

extern "C" void WINAPI proxy_D3DPERF_SetOptions(DWORD dwOptions) {
    EnsureRealD3D9Loaded();
    if (!g_D3DPERF_SetOptions) return;
    auto fn = reinterpret_cast<void(WINAPI*)(DWORD)>(g_D3DPERF_SetOptions);
    fn(dwOptions);
}

extern "C" void WINAPI proxy_D3DPERF_SetRegion(DWORD col, LPCWSTR wszName) {
    EnsureRealD3D9Loaded();
    if (!g_D3DPERF_SetRegion) return;
    auto fn = reinterpret_cast<void(WINAPI*)(DWORD, LPCWSTR)>(g_D3DPERF_SetRegion);
    fn(col, wszName);
}

// Undocumented/private exports: ABI-transparent x86 trampolines
// These preserve all registers, flags, and stack verbatim, then tail-jump.
// If resolution fails, fail-fast instead of jumping null.

static const char str_Direct3DShaderValidatorCreate9[] = "Direct3DShaderValidatorCreate9";
extern "C" __declspec(naked) void WINAPI proxy_Direct3DShaderValidatorCreate9() {
    __asm push ebp
    __asm mov ebp, esp
    __asm call EnsureRealD3D9Loaded
    __asm mov eax, dword ptr [g_Direct3DShaderValidatorCreate9]
    __asm test eax, eax
    __asm jz lbl_fail_shadervalidator
    __asm pop ebp
    __asm jmp eax
lbl_fail_shadervalidator:
    __asm push offset str_Direct3DShaderValidatorCreate9
    __asm call FailFastMissingExport
}

static const char str_PSGPError[] = "PSGPError";
extern "C" __declspec(naked) void WINAPI proxy_PSGPError() {
    __asm push ebp
    __asm mov ebp, esp
    __asm call EnsureRealD3D9Loaded
    __asm mov eax, dword ptr [g_PSGPError]
    __asm test eax, eax
    __asm jz lbl_fail_psgperror
    __asm pop ebp
    __asm jmp eax
lbl_fail_psgperror:
    __asm push offset str_PSGPError
    __asm call FailFastMissingExport
}

static const char str_PSGPSampleTexture[] = "PSGPSampleTexture";
extern "C" __declspec(naked) void WINAPI proxy_PSGPSampleTexture() {
    __asm push ebp
    __asm mov ebp, esp
    __asm call EnsureRealD3D9Loaded
    __asm mov eax, dword ptr [g_PSGPSampleTexture]
    __asm test eax, eax
    __asm jz lbl_fail_psgpsampletexture
    __asm pop ebp
    __asm jmp eax
lbl_fail_psgpsampletexture:
    __asm push offset str_PSGPSampleTexture
    __asm call FailFastMissingExport
}

static const char str_DebugSetLevel[] = "DebugSetLevel";
extern "C" __declspec(naked) void WINAPI proxy_DebugSetLevel() {
    __asm push ebp
    __asm mov ebp, esp
    __asm call EnsureRealD3D9Loaded
    __asm mov eax, dword ptr [g_DebugSetLevel]
    __asm test eax, eax
    __asm jz lbl_fail_debugsetlevel
    __asm pop ebp
    __asm jmp eax
lbl_fail_debugsetlevel:
    __asm push offset str_DebugSetLevel
    __asm call FailFastMissingExport
}

static const char str_DebugSetMute[] = "DebugSetMute";
extern "C" __declspec(naked) void WINAPI proxy_DebugSetMute() {
    __asm push ebp
    __asm mov ebp, esp
    __asm call EnsureRealD3D9Loaded
    __asm mov eax, dword ptr [g_DebugSetMute]
    __asm test eax, eax
    __asm jz lbl_fail_debugsetmute
    __asm pop ebp
    __asm jmp eax
lbl_fail_debugsetmute:
    __asm push offset str_DebugSetMute
    __asm call FailFastMissingExport
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class ProxyIDirect3D9;
class ProxyIDirect3DDevice9;

// ---------------------------------------------------------------------------
// ProxyIDirect3DSwapChain9: IDirect3DSwapChain9 wrapper with Present telemetry
// ---------------------------------------------------------------------------
class ProxyIDirect3DSwapChain9 : public IDirect3DSwapChain9 {
public:
    ProxyIDirect3DSwapChain9(IDirect3DSwapChain9* inner, UINT swapChainIndex)
        : m_inner(inner), m_refs(1), m_swapChainIndex(swapChainIndex), m_presentCount(0) {
        Log("[swapchain] IDirect3DSwapChain9 wrapped (inner=0x%p, index=%u)\n", inner, swapChainIndex);
    }

    ~ProxyIDirect3DSwapChain9() {
        Log("[swapchain] destroyed (index=%u, presents=%u)\n", m_swapChainIndex, m_presentCount);
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) {
        if (ppvObj == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IDirect3DSwapChain9)) {
            AddRef();
            *ppvObj = static_cast<IDirect3DSwapChain9*>(this);
            return S_OK;
        }
        return m_inner->QueryInterface(riid, ppvObj);
    }

    STDMETHOD_(ULONG, AddRef)() {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    STDMETHOD_(ULONG, Release)() {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) {
            m_inner->Release();
            delete this;
        }
        return static_cast<ULONG>(r);
    }

    // IDirect3DSwapChain9
    STDMETHOD(Present)(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                       HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
        HRESULT hr = m_inner->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        m_presentCount++;
        if (m_presentCount <= 8 || FAILED(hr)) {
            Log("[swapchain] Present: hr=0x%08lx, index=%u, call=%u, hwnd=0x%p\n",
                static_cast<unsigned long>(hr), m_swapChainIndex, m_presentCount, hDestWindowOverride);
        }
        return hr;
    }

    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9* pDestSurface) {
        HRESULT hr = m_inner->GetFrontBufferData(pDestSurface);
        if (m_presentCount <= 8 || FAILED(hr)) {
            Log("[swapchain] GetFrontBufferData: hr=0x%08lx, index=%u\n",
                static_cast<unsigned long>(hr), m_swapChainIndex);
        }
        return hr;
    }

    STDMETHOD(GetBackBuffer)(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) {
        HRESULT hr = m_inner->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
        if (m_presentCount <= 8 || FAILED(hr)) {
            Log("[swapchain] GetBackBuffer: hr=0x%08lx, index=%u, backbuffer=%u\n",
                static_cast<unsigned long>(hr), m_swapChainIndex, iBackBuffer);
        }
        return hr;
    }

    STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS* pRasterStatus) {
        return m_inner->GetRasterStatus(pRasterStatus);
    }

    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE* pMode) {
        return m_inner->GetDisplayMode(pMode);
    }

    STDMETHOD(GetDevice)(IDirect3DDevice9** ppDevice) {
        return m_inner->GetDevice(ppDevice);
    }

    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS* pPresentationParameters) {
        HRESULT hr = m_inner->GetPresentParameters(pPresentationParameters);
        if (m_presentCount <= 8 || FAILED(hr)) {
            Log("[swapchain] GetPresentParameters: hr=0x%08lx, index=%u\n",
                static_cast<unsigned long>(hr), m_swapChainIndex);
        }
        return hr;
    }

private:
    IDirect3DSwapChain9* m_inner;
    volatile LONG m_refs;
    UINT m_swapChainIndex;
    UINT m_presentCount;
};

// ---------------------------------------------------------------------------
// ProxyIDirect3DDevice9: IDirect3DDevice9 wrapper with telemetry and capture.
// ---------------------------------------------------------------------------
class ProxyIDirect3DDevice9 : public IDirect3DDevice9 {
public:
    ProxyIDirect3DDevice9(IDirect3DDevice9* inner, ProxyIDirect3D9* parent,
                          const DeviceDiagnosticsConfig& config, bool on12DeviceVerified)
        : m_inner(inner), m_parent(parent), m_refs(1), m_config(config),
          m_presentCount(0), m_captureAttempted(false),
          m_on12DeviceVerified(on12DeviceVerified), m_testMarkerLogged(false) {
        ZeroMemory(&m_counters, sizeof(m_counters));
        AddRefParent();
        Log("[device] IDirect3DDevice9 wrapped (inner=0x%p, parent=0x%p)\n", inner, parent);
    }

    ~ProxyIDirect3DDevice9() {
        Log("[device] destroyed after %u presents; final counters:\n", m_presentCount);
        Log("  TestCooperativeLevel=%u GetDeviceCaps=%u Reset=%u Present=%u\n",
            m_counters.testCooperativeLevel, m_counters.getDeviceCaps,
            m_counters.reset, m_counters.present);
        Log("  BeginScene=%u EndScene=%u Clear=%u\n",
            m_counters.beginScene, m_counters.endScene, m_counters.clear);
        Log("  CreateTexture=%u CreateRenderTarget=%u CreateDepthStencilSurface=%u\n",
            m_counters.createTexture, m_counters.createRenderTarget,
            m_counters.createDepthStencilSurface);
        Log("  SetRenderTarget=%u SetDepthStencilSurface=%u\n",
            m_counters.setRenderTarget, m_counters.setDepthStencilSurface);
        Log("  DrawPrimitive=%u DrawIndexedPrimitive=%u DrawPrimitiveUP=%u DrawIndexedPrimitiveUP=%u\n",
            m_counters.drawPrimitive, m_counters.drawIndexedPrimitive,
            m_counters.drawPrimitiveUP, m_counters.drawIndexedPrimitiveUP);
        Log("  CreateVertexShader=%u SetVertexShader=%u CreatePixelShader=%u SetPixelShader=%u\n",
            m_counters.createVertexShader, m_counters.setVertexShader,
            m_counters.createPixelShader, m_counters.setPixelShader);
        ReleaseParent();
    }

    void AddRefParent();
    void ReleaseParent();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObj) {
        if (ppvObj == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIID_IDirect3DDevice9) {
            AddRef();
            *ppvObj = static_cast<IDirect3DDevice9*>(this);
            return S_OK;
        }
        // Forward other IIDs (including IDirect3DDevice9On12) to inner
        return m_inner->QueryInterface(riid, ppvObj);
    }

    STDMETHOD_(ULONG, AddRef)() {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    STDMETHOD_(ULONG, Release)() {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) {
            if (m_on12DeviceVerified) {
                LogFinalD3D12DeviceRemovedReason(m_inner);
            }
            m_inner->Release();
            delete this;
        }
        return static_cast<ULONG>(r);
    }

    // IDirect3DDevice9 methods (generated include)
#include "device9_methods.generated.inc"

private:
    // Telemetry counters
    struct Counters {
        UINT testCooperativeLevel;
        UINT getDeviceCaps;
        UINT reset;
        UINT present;
        UINT beginScene;
        UINT endScene;
        UINT clear;
        UINT createTexture;
        UINT createRenderTarget;
        UINT createDepthStencilSurface;
        UINT setRenderTarget;
        UINT setDepthStencilSurface;
        UINT drawPrimitive;
        UINT drawIndexedPrimitive;
        UINT drawPrimitiveUP;
        UINT drawIndexedPrimitiveUP;
        UINT createVertexShader;
        UINT setVertexShader;
        UINT createPixelShader;
        UINT setPixelShader;
    };

    IDirect3DDevice9* m_inner;
    ProxyIDirect3D9* m_parent;
    volatile LONG m_refs;
    DeviceDiagnosticsConfig m_config;
    Counters m_counters;
    UINT m_presentCount;
    bool m_captureAttempted;
    bool m_on12DeviceVerified;
    bool m_testMarkerLogged;

    // Throttle routine telemetry: log every Nth call
    static const UINT kThrottleInterval = 1000;

    bool ShouldTrace(UINT counter) const {
        return m_config.traceDevice && (counter % kThrottleInterval == 0);
    }

    void ApplyTestMarker() {
        if (!m_config.testMarker) return;

        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = m_inner->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            if (!m_testMarkerLogged) {
                Log("[mod] test marker GetBackBuffer failed: hr=0x%08lx\n",
                    static_cast<unsigned long>(hr));
                m_testMarkerLogged = true;
            }
            return;
        }

        D3DSURFACE_DESC desc = {};
        hr = backBuffer->GetDesc(&desc);
        if (SUCCEEDED(hr)) {
            const LONG width = static_cast<LONG>(desc.Width);
            const LONG height = static_cast<LONG>(desc.Height);
            const LONG markerSize = static_cast<LONG>(m_config.testMarkerSize);
            const LONG margin = 8;
            RECT rect = {
                width > markerSize + margin ? width - markerSize - margin : 0,
                height > markerSize + margin ? height - markerSize - margin : 0,
                width > margin ? width - margin : width,
                height > margin ? height - margin : height};
            hr = m_inner->ColorFill(
                backBuffer, &rect, D3DCOLOR_ARGB(255, 255, 0, 255));
        }
        backBuffer->Release();

        if (!m_testMarkerLogged) {
            Log("[mod] test marker %s: hr=0x%08lx, color=0xffff00ff\n",
                SUCCEEDED(hr) ? "applied" : "failed", static_cast<unsigned long>(hr));
            m_testMarkerLogged = true;
        }
    }

    // Intercept implementations
    HRESULT Intercept_TestCooperativeLevel() {
        HRESULT hr = m_inner->TestCooperativeLevel();
        m_counters.testCooperativeLevel++;
        if (FAILED(hr) || m_counters.testCooperativeLevel <= 8) {
            Log("[device] TestCooperativeLevel: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.testCooperativeLevel);
        } else if (ShouldTrace(m_counters.testCooperativeLevel)) {
            Log("[device] TestCooperativeLevel: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.testCooperativeLevel);
        }
        return hr;
    }

    HRESULT Intercept_GetDeviceCaps(D3DCAPS9* pCaps) {
        HRESULT hr = m_inner->GetDeviceCaps(pCaps);
        m_counters.getDeviceCaps++;
        if (FAILED(hr)) {
            Log("[device] GetDeviceCaps failed: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.getDeviceCaps);
        } else if (ShouldTrace(m_counters.getDeviceCaps)) {
            Log("[device] GetDeviceCaps: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.getDeviceCaps);
        }
        return hr;
    }

    HRESULT Intercept_Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
        HRESULT hr = m_inner->Reset(pPresentationParameters);
        m_counters.reset++;
        if (FAILED(hr) || m_counters.reset <= 8) {
            Log("[device] Reset: hr=0x%08lx, bb=%ux%u fmt=%u (call %u)\n",
                static_cast<unsigned long>(hr),
                pPresentationParameters ? pPresentationParameters->BackBufferWidth : 0,
                pPresentationParameters ? pPresentationParameters->BackBufferHeight : 0,
                pPresentationParameters ? pPresentationParameters->BackBufferFormat : 0,
                m_counters.reset);
            if (SUCCEEDED(hr)) {
                m_captureAttempted = false;
            }
        }
        return hr;
    }

    HRESULT Intercept_Present(CONST RECT* pSourceRect, CONST RECT* pDestRect,
                              HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
        ApplyTestMarker();

        // Backbuffer capture before present if configured
        bool doCapture = m_config.captureFrames && !m_captureAttempted &&
                         m_presentCount == m_config.captureFrame;
        if (doCapture) {
            CaptureBackbuffer();
        }

        // Call real Present exactly once
        HRESULT hr = m_inner->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        m_presentCount++;
        m_counters.present++;

        // Log first call with creation parameters and swap chain info
        if (m_counters.present == 1) {
            LogFirstPresent();
        }

        // Frontbuffer capture after present if configured and Present succeeded
        if (doCapture) {
            if (SUCCEEDED(hr) && m_config.captureFrontBuffer) {
                Log("[capture] post-Present frontbuffer (hr=0x%08lx, frame=%u, call=%u)\n",
                    static_cast<unsigned long>(hr), m_presentCount, m_counters.present);
                CaptureFrontbuffer();
            } else if (FAILED(hr)) {
                Log("[capture] skipping frontbuffer capture (Present failed: hr=0x%08lx)\n",
                    static_cast<unsigned long>(hr));
            }
            m_captureAttempted = true;
        }

        if (FAILED(hr) || m_counters.present <= 8) {
            Log("[device] Present: hr=0x%08lx (frame %u, call %u, hwnd=0x%p)\n",
                static_cast<unsigned long>(hr), m_presentCount, m_counters.present, hDestWindowOverride);
        } else if (ShouldTrace(m_counters.present)) {
            Log("[device] Present: hr=0x%08lx (frame %u, call %u)\n",
                static_cast<unsigned long>(hr), m_presentCount, m_counters.present);
        }
        return hr;
    }

    HRESULT Intercept_BeginScene() {
        HRESULT hr = m_inner->BeginScene();
        m_counters.beginScene++;
        if (FAILED(hr) || m_counters.beginScene <= 8) {
            Log("[device] BeginScene: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.beginScene);
        } else if (ShouldTrace(m_counters.beginScene)) {
            Log("[device] BeginScene: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.beginScene);
        }
        return hr;
    }

    HRESULT Intercept_EndScene() {
        HRESULT hr = m_inner->EndScene();
        m_counters.endScene++;
        if (FAILED(hr) || m_counters.endScene <= 8) {
            Log("[device] EndScene: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.endScene);
        } else if (ShouldTrace(m_counters.endScene)) {
            Log("[device] EndScene: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.endScene);
        }
        return hr;
    }

    HRESULT Intercept_Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags,
                            D3DCOLOR Color, float Z, DWORD Stencil) {
        HRESULT hr = m_inner->Clear(Count, pRects, Flags, Color, Z, Stencil);
        m_counters.clear++;
        if (FAILED(hr)) {
            Log("[device] Clear failed: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.clear);
        } else if (ShouldTrace(m_counters.clear)) {
            Log("[device] Clear: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.clear);
        }
        return hr;
    }

    HRESULT Intercept_CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                    D3DFORMAT Format, D3DPOOL Pool,
                                    IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
        HRESULT hr = m_inner->CreateTexture(Width, Height, Levels, Usage, Format, Pool,
                                            ppTexture, pSharedHandle);
        m_counters.createTexture++;
        if (FAILED(hr)) {
            Log("[device] CreateTexture failed: hr=0x%08lx, %ux%u fmt=%u (call %u)\n",
                static_cast<unsigned long>(hr), Width, Height, Format,
                m_counters.createTexture);
        } else if (ShouldTrace(m_counters.createTexture)) {
            Log("[device] CreateTexture: hr=0x%08lx, %ux%u fmt=%u (call %u)\n",
                static_cast<unsigned long>(hr), Width, Height, Format,
                m_counters.createTexture);
        }
        return hr;
    }

    HRESULT Intercept_CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                         D3DMULTISAMPLE_TYPE MultiSample,
                                         DWORD MultisampleQuality, BOOL Lockable,
                                         IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
        HRESULT hr = m_inner->CreateRenderTarget(Width, Height, Format, MultiSample,
                                                 MultisampleQuality, Lockable,
                                                 ppSurface, pSharedHandle);
        m_counters.createRenderTarget++;
        if (FAILED(hr)) {
            Log("[device] CreateRenderTarget failed: hr=0x%08lx, %ux%u fmt=%u (call %u)\n",
                static_cast<unsigned long>(hr), Width, Height, Format,
                m_counters.createRenderTarget);
        } else if (ShouldTrace(m_counters.createRenderTarget)) {
            Log("[device] CreateRenderTarget: hr=0x%08lx, %ux%u fmt=%u (call %u)\n",
                static_cast<unsigned long>(hr), Width, Height, Format,
                m_counters.createRenderTarget);
        }
        return hr;
    }

    HRESULT Intercept_CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                D3DMULTISAMPLE_TYPE MultiSample,
                                                DWORD MultisampleQuality, BOOL Discard,
                                                IDirect3DSurface9** ppSurface,
                                                HANDLE* pSharedHandle) {
        HRESULT hr = m_inner->CreateDepthStencilSurface(Width, Height, Format, MultiSample,
                                                        MultisampleQuality, Discard,
                                                        ppSurface, pSharedHandle);
        m_counters.createDepthStencilSurface++;
        if (FAILED(hr)) {
            Log("[device] CreateDepthStencilSurface failed: hr=0x%08lx, %ux%u fmt=%u (call %u)\n",
                static_cast<unsigned long>(hr), Width, Height, Format,
                m_counters.createDepthStencilSurface);
        } else if (ShouldTrace(m_counters.createDepthStencilSurface)) {
            Log("[device] CreateDepthStencilSurface: hr=0x%08lx, %ux%u fmt=%u (call %u)\n",
                static_cast<unsigned long>(hr), Width, Height, Format,
                m_counters.createDepthStencilSurface);
        }
        return hr;
    }

    HRESULT Intercept_SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
        HRESULT hr = m_inner->SetRenderTarget(RenderTargetIndex, pRenderTarget);
        m_counters.setRenderTarget++;
        if (FAILED(hr)) {
            Log("[device] SetRenderTarget failed: hr=0x%08lx, index=%lu (call %u)\n",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(RenderTargetIndex),
                m_counters.setRenderTarget);
        } else if (ShouldTrace(m_counters.setRenderTarget)) {
            Log("[device] SetRenderTarget: hr=0x%08lx, index=%lu (call %u)\n",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(RenderTargetIndex),
                m_counters.setRenderTarget);
        }
        return hr;
    }

    HRESULT Intercept_SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
        HRESULT hr = m_inner->SetDepthStencilSurface(pNewZStencil);
        m_counters.setDepthStencilSurface++;
        if (FAILED(hr)) {
            Log("[device] SetDepthStencilSurface failed: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.setDepthStencilSurface);
        } else if (ShouldTrace(m_counters.setDepthStencilSurface)) {
            Log("[device] SetDepthStencilSurface: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.setDepthStencilSurface);
        }
        return hr;
    }

    HRESULT Intercept_DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                    UINT PrimitiveCount) {
        HRESULT hr = m_inner->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
        m_counters.drawPrimitive++;
        if (FAILED(hr)) {
            Log("[device] DrawPrimitive failed: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, PrimitiveCount,
                m_counters.drawPrimitive);
        } else if (ShouldTrace(m_counters.drawPrimitive)) {
            Log("[device] DrawPrimitive: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, PrimitiveCount,
                m_counters.drawPrimitive);
        }
        return hr;
    }

    HRESULT Intercept_DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                           INT BaseVertexIndex, UINT MinVertexIndex,
                                           UINT NumVertices, UINT startIndex, UINT primCount) {
        HRESULT hr = m_inner->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex,
                                                   MinVertexIndex, NumVertices,
                                                   startIndex, primCount);
        m_counters.drawIndexedPrimitive++;
        if (FAILED(hr)) {
            Log("[device] DrawIndexedPrimitive failed: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, primCount,
                m_counters.drawIndexedPrimitive);
        } else if (ShouldTrace(m_counters.drawIndexedPrimitive)) {
            Log("[device] DrawIndexedPrimitive: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, primCount,
                m_counters.drawIndexedPrimitive);
        }
        return hr;
    }

    HRESULT Intercept_DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                      CONST void* pVertexStreamZeroData,
                                      UINT VertexStreamZeroStride) {
        HRESULT hr = m_inner->DrawPrimitiveUP(PrimitiveType, PrimitiveCount,
                                              pVertexStreamZeroData, VertexStreamZeroStride);
        m_counters.drawPrimitiveUP++;
        if (FAILED(hr)) {
            Log("[device] DrawPrimitiveUP failed: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, PrimitiveCount,
                m_counters.drawPrimitiveUP);
        } else if (ShouldTrace(m_counters.drawPrimitiveUP)) {
            Log("[device] DrawPrimitiveUP: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, PrimitiveCount,
                m_counters.drawPrimitiveUP);
        }
        return hr;
    }

    HRESULT Intercept_DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                             UINT MinVertexIndex, UINT NumVertices,
                                             UINT PrimitiveCount, CONST void* pIndexData,
                                             D3DFORMAT IndexDataFormat,
                                             CONST void* pVertexStreamZeroData,
                                             UINT VertexStreamZeroStride) {
        HRESULT hr = m_inner->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices,
                                                     PrimitiveCount, pIndexData, IndexDataFormat,
                                                     pVertexStreamZeroData, VertexStreamZeroStride);
        m_counters.drawIndexedPrimitiveUP++;
        if (FAILED(hr)) {
            Log("[device] DrawIndexedPrimitiveUP failed: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, PrimitiveCount,
                m_counters.drawIndexedPrimitiveUP);
        } else if (ShouldTrace(m_counters.drawIndexedPrimitiveUP)) {
            Log("[device] DrawIndexedPrimitiveUP: hr=0x%08lx, type=%u count=%u (call %u)\n",
                static_cast<unsigned long>(hr), PrimitiveType, PrimitiveCount,
                m_counters.drawIndexedPrimitiveUP);
        }
        return hr;
    }

    HRESULT Intercept_GetDirect3D(IDirect3D9** ppD3D9);

    HRESULT Intercept_CreateVertexShader(CONST DWORD* pFunction,
                                         IDirect3DVertexShader9** ppShader) {
        HRESULT hr = m_inner->CreateVertexShader(pFunction, ppShader);
        m_counters.createVertexShader++;
        if (FAILED(hr)) {
            Log("[device] CreateVertexShader failed: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.createVertexShader);
        } else if (ShouldTrace(m_counters.createVertexShader)) {
            Log("[device] CreateVertexShader: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.createVertexShader);
        }
        return hr;
    }

    HRESULT Intercept_SetVertexShader(IDirect3DVertexShader9* pShader) {
        HRESULT hr = m_inner->SetVertexShader(pShader);
        m_counters.setVertexShader++;
        if (FAILED(hr)) {
            Log("[device] SetVertexShader failed: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.setVertexShader);
        } else if (ShouldTrace(m_counters.setVertexShader)) {
            Log("[device] SetVertexShader: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.setVertexShader);
        }
        return hr;
    }

    HRESULT Intercept_CreatePixelShader(CONST DWORD* pFunction,
                                        IDirect3DPixelShader9** ppShader) {
        HRESULT hr = m_inner->CreatePixelShader(pFunction, ppShader);
        m_counters.createPixelShader++;
        if (FAILED(hr)) {
            Log("[device] CreatePixelShader failed: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.createPixelShader);
        } else if (ShouldTrace(m_counters.createPixelShader)) {
            Log("[device] CreatePixelShader: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.createPixelShader);
        }
        return hr;
    }

    HRESULT Intercept_SetPixelShader(IDirect3DPixelShader9* pShader) {
        HRESULT hr = m_inner->SetPixelShader(pShader);
        m_counters.setPixelShader++;
        if (FAILED(hr)) {
            Log("[device] SetPixelShader failed: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.setPixelShader);
        } else if (ShouldTrace(m_counters.setPixelShader)) {
            Log("[device] SetPixelShader: hr=0x%08lx (call %u)\n",
                static_cast<unsigned long>(hr), m_counters.setPixelShader);
        }
        return hr;
    }

    HRESULT Intercept_GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
        if (!pSwapChain) return D3DERR_INVALIDCALL;
        *pSwapChain = nullptr;

        IDirect3DSwapChain9* innerSwapChain = nullptr;
        HRESULT hr = m_inner->GetSwapChain(iSwapChain, &innerSwapChain);

        if (m_counters.present <= 8 || FAILED(hr)) {
            Log("[device] GetSwapChain: hr=0x%08lx, index=%u\n",
                static_cast<unsigned long>(hr), iSwapChain);
        }

        if (SUCCEEDED(hr) && innerSwapChain) {
            ProxyIDirect3DSwapChain9* proxySwapChain = new (std::nothrow) ProxyIDirect3DSwapChain9(
                innerSwapChain, iSwapChain);
            if (!proxySwapChain) {
                Log("[device] swap chain wrapper allocation failed\n");
                innerSwapChain->Release();
                return E_OUTOFMEMORY;
            }
            *pSwapChain = proxySwapChain;
        }
        return hr;
    }

    HRESULT Intercept_CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                IDirect3DSwapChain9** pSwapChain) {
        if (!pSwapChain) return D3DERR_INVALIDCALL;
        *pSwapChain = nullptr;

        IDirect3DSwapChain9* innerSwapChain = nullptr;
        HRESULT hr = m_inner->CreateAdditionalSwapChain(pPresentationParameters, &innerSwapChain);

        if (m_counters.present <= 8 || FAILED(hr)) {
            Log("[device] CreateAdditionalSwapChain: hr=0x%08lx\n",
                static_cast<unsigned long>(hr));
        }

        if (SUCCEEDED(hr) && innerSwapChain) {
            ProxyIDirect3DSwapChain9* proxySwapChain = new (std::nothrow) ProxyIDirect3DSwapChain9(
                innerSwapChain, 999); // Additional chains don't have fixed indices
            if (!proxySwapChain) {
                Log("[device] swap chain wrapper allocation failed\n");
                innerSwapChain->Release();
                return E_OUTOFMEMORY;
            }
            *pSwapChain = proxySwapChain;
        }
        return hr;
    }

    HRESULT Intercept_GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
        HRESULT hr = m_inner->GetFrontBufferData(iSwapChain, pDestSurface);
        if (m_counters.present <= 8 || FAILED(hr)) {
            Log("[device] GetFrontBufferData: hr=0x%08lx, swapchain=%u\n",
                static_cast<unsigned long>(hr), iSwapChain);
        }
        return hr;
    }

    // Backbuffer capture with BMP writing and luminance analysis
    void CaptureBackbuffer() {
        Log("[capture] attempting backbuffer capture at frame %u\n", m_presentCount);

        IDirect3DSurface9* backbuffer = nullptr;
        HRESULT hr = m_inner->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        if (FAILED(hr) || !backbuffer) {
            Log("[capture] GetBackBuffer failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
            return;
        }

        D3DSURFACE_DESC desc;
        hr = backbuffer->GetDesc(&desc);
        if (FAILED(hr)) {
            Log("[capture] GetDesc failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
            backbuffer->Release();
            return;
        }

        Log("[capture] backbuffer: %ux%u, format=%u\n", desc.Width, desc.Height, desc.Format);

        // Only support X8R8G8B8 and A8R8G8B8
        if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
            Log("[capture] unsupported format %u (expected X8R8G8B8=22 or A8R8G8B8=21)\n",
                desc.Format);
            backbuffer->Release();
            return;
        }

        // Create offscreen surface in system memory
        IDirect3DSurface9* offscreen = nullptr;
        hr = m_inner->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                  D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
        if (FAILED(hr) || !offscreen) {
            Log("[capture] CreateOffscreenPlainSurface failed: hr=0x%08lx\n",
                static_cast<unsigned long>(hr));
            backbuffer->Release();
            return;
        }

        // Copy render target data to offscreen surface
        hr = m_inner->GetRenderTargetData(backbuffer, offscreen);
        backbuffer->Release();
        if (FAILED(hr)) {
            Log("[capture] GetRenderTargetData failed: hr=0x%08lx\n",
                static_cast<unsigned long>(hr));
            offscreen->Release();
            return;
        }

        // Lock the offscreen surface
        D3DLOCKED_RECT lockedRect;
        hr = offscreen->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            Log("[capture] LockRect failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
            offscreen->Release();
            return;
        }

        Log("[capture] locked: pitch=%d\n", lockedRect.Pitch);

        // Write BMP and compute luminance
        WriteBMPAndAnalyze(lockedRect.pBits, desc.Width, desc.Height, lockedRect.Pitch,
                          "bully_renderprobe_backbuffer.bmp");

        offscreen->UnlockRect();
        offscreen->Release();
    }

    void LogFirstPresent() {
        Log("[device] first Present - querying creation parameters and swap chain info\n");

        D3DDEVICE_CREATION_PARAMETERS creationParams;
        HRESULT hr = m_inner->GetCreationParameters(&creationParams);
        if (SUCCEEDED(hr)) {
            Log("[device]   GetCreationParameters: Adapter=%u, DeviceType=%u, hFocusWindow=0x%p, BehaviorFlags=0x%x\n",
                creationParams.AdapterOrdinal, creationParams.DeviceType,
                creationParams.hFocusWindow, creationParams.BehaviorFlags);
        } else {
            Log("[device]   GetCreationParameters failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
        }

        IDirect3DSwapChain9* swapChain = nullptr;
        hr = m_inner->GetSwapChain(0, &swapChain);
        if (SUCCEEDED(hr) && swapChain) {
            D3DPRESENT_PARAMETERS pp;
            hr = swapChain->GetPresentParameters(&pp);
            if (SUCCEEDED(hr)) {
                Log("[device]   Implicit SwapChain(0) PresentParameters:\n");
                Log("[device]     BackBuffer: %ux%u, Format=%u, Count=%u\n",
                    pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, pp.BackBufferCount);
                Log("[device]     MultiSample: Type=%u, Quality=%u\n",
                    pp.MultiSampleType, pp.MultiSampleQuality);
                Log("[device]     SwapEffect=%u, hDeviceWindow=0x%p, Windowed=%d\n",
                    pp.SwapEffect, pp.hDeviceWindow, pp.Windowed);
                Log("[device]     AutoDepthStencil: Enable=%d, Format=%u\n",
                    pp.EnableAutoDepthStencil, pp.AutoDepthStencilFormat);
                Log("[device]     Flags=0x%x, FullScreen_RefreshRateInHz=%u, PresentationInterval=%u\n",
                    pp.Flags, pp.FullScreen_RefreshRateInHz, pp.PresentationInterval);
            } else {
                Log("[device]   GetPresentParameters failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
            }
            swapChain->Release();
        } else {
            Log("[device]   GetSwapChain(0) failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
        }
    }

        void CaptureFrontbuffer() {
            Log("[capture] attempting frontbuffer capture at frame %u\n", m_presentCount);

            // Get the implicit swap chain to query presentation parameters
            IDirect3DSwapChain9* swapChain = nullptr;
            HRESULT hr = m_inner->GetSwapChain(0, &swapChain);
            if (FAILED(hr) || !swapChain) {
                Log("[capture] GetSwapChain(0) failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
                return;
            }

            D3DPRESENT_PARAMETERS pp = {};
            hr = swapChain->GetPresentParameters(&pp);
            if (FAILED(hr)) {
                Log("[capture] GetPresentParameters failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
                swapChain->Release();
                return;
            }

            Log("[capture] source swap chain: %ux%u, BackBufferFormat=%u, Windowed=%d\n",
                pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, pp.Windowed);

            // Determine correct destination dimensions based on windowed vs fullscreen
            UINT destWidth, destHeight;
            if (pp.Windowed) {
                // Windowed mode: use desktop dimensions
                destWidth = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
                destHeight = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
                Log("[capture] windowed mode: using desktop dimensions %ux%u\n", destWidth, destHeight);
            } else {
                // Fullscreen mode: use display mode dimensions
                D3DDISPLAYMODE displayMode = {};
                hr = swapChain->GetDisplayMode(&displayMode);
                if (FAILED(hr)) {
                    Log("[capture] GetDisplayMode failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
                    swapChain->Release();
                    return;
                }
                destWidth = displayMode.Width;
                destHeight = displayMode.Height;
                Log("[capture] fullscreen mode: using display dimensions %ux%u\n", destWidth, destHeight);
            }

            swapChain->Release();

            // Use A8R8G8B8 as documented front buffer destination format
            const D3DFORMAT destFormat = D3DFMT_A8R8G8B8;
            Log("[capture] destination surface: %ux%u, format=A8R8G8B8 (21)\n", destWidth, destHeight);

            // Create offscreen surface in system memory
            IDirect3DSurface9* offscreen = nullptr;
            hr = m_inner->CreateOffscreenPlainSurface(destWidth, destHeight, destFormat,
                                                      D3DPOOL_SYSTEMMEM, &offscreen, nullptr);
            if (FAILED(hr) || !offscreen) {
                Log("[capture] CreateOffscreenPlainSurface(%ux%u, fmt=%u) failed: hr=0x%08lx\n",
                    destWidth, destHeight, destFormat, static_cast<unsigned long>(hr));
                return;
            }

            // Copy front buffer data to offscreen surface
            hr = m_inner->GetFrontBufferData(0, offscreen);
            Log("[capture] GetFrontBufferData(0, offscreen) returned: hr=0x%08lx\n",
                static_cast<unsigned long>(hr));
            if (FAILED(hr)) {
                offscreen->Release();
                return;
            }

            // Lock the offscreen surface
            D3DLOCKED_RECT lockedRect;
            hr = offscreen->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
            if (FAILED(hr)) {
                Log("[capture] LockRect failed: hr=0x%08lx\n", static_cast<unsigned long>(hr));
                offscreen->Release();
                return;
            }

            Log("[capture] locked: pitch=%d\n", lockedRect.Pitch);

            // Write BMP and compute luminance
            WriteBMPAndAnalyze(lockedRect.pBits, destWidth, destHeight,
                              lockedRect.Pitch, "bully_renderprobe_frontbuffer.bmp");

            offscreen->UnlockRect();
            offscreen->Release();
        }

    void WriteBMPAndAnalyze(void* pixels, UINT width, UINT height, INT pitch, const char* filename) {
        // Validate dimensions
        if (width == 0 || height == 0) {
            Log("[capture] invalid dimensions: %ux%u\n", width, height);
            return;
        }
        if (pixels == nullptr) {
            Log("[capture] null pixel data\n");
            return;
        }
        if (pitch == 0) {
            Log("[capture] zero pitch\n");
            return;
        }

        // Check for overflow in size calculations
        const UINT rowBytes = width * 4;
        if (rowBytes / 4 != width) {
            Log("[capture] rowBytes overflow\n");
            return;
        }
        const UINT imageSize = rowBytes * height;
        if (imageSize / height != rowBytes) {
            Log("[capture] imageSize overflow\n");
            return;
        }

        char bmpPath[MAX_PATH];
        if (!GetExeSiblingPath(filename, bmpPath, sizeof(bmpPath))) {
            Log("[capture] could not resolve BMP path\n");
            return;
        }

        FILE* f = nullptr;
        errno_t err = fopen_s(&f, bmpPath, "wb");
        if (err != 0 || !f) {
            Log("[capture] fopen failed: errno=%d\n", err);
            return;
        }

        // BMP header (54 bytes)
        const UINT fileSize = 54 + imageSize;

        // BITMAPFILEHEADER (14 bytes)
        unsigned char fileHeader[14] = {
            'B', 'M',                    // signature
            static_cast<unsigned char>(fileSize & 0xFF),
            static_cast<unsigned char>((fileSize >> 8) & 0xFF),
            static_cast<unsigned char>((fileSize >> 16) & 0xFF),
            static_cast<unsigned char>((fileSize >> 24) & 0xFF),
            0, 0, 0, 0,                  // reserved
            54, 0, 0, 0                  // pixel data offset
        };
        fwrite(fileHeader, 1, 14, f);

        // BITMAPINFOHEADER (40 bytes)
        unsigned char infoHeader[40] = {
            40, 0, 0, 0,                 // header size
            static_cast<unsigned char>(width & 0xFF),
            static_cast<unsigned char>((width >> 8) & 0xFF),
            static_cast<unsigned char>((width >> 16) & 0xFF),
            static_cast<unsigned char>((width >> 24) & 0xFF),
            static_cast<unsigned char>(height & 0xFF),
            static_cast<unsigned char>((height >> 8) & 0xFF),
            static_cast<unsigned char>((height >> 16) & 0xFF),
            static_cast<unsigned char>((height >> 24) & 0xFF),
            1, 0,                        // planes
            32, 0,                       // bits per pixel
            0, 0, 0, 0,                  // compression (BI_RGB)
            static_cast<unsigned char>(imageSize & 0xFF),
            static_cast<unsigned char>((imageSize >> 8) & 0xFF),
            static_cast<unsigned char>((imageSize >> 16) & 0xFF),
            static_cast<unsigned char>((imageSize >> 24) & 0xFF),
            0, 0, 0, 0,                  // X pixels per meter
            0, 0, 0, 0,                  // Y pixels per meter
            0, 0, 0, 0,                  // colors used
            0, 0, 0, 0                   // colors important
        };
        fwrite(infoHeader, 1, 40, f);

        // Write pixel data (bottom-up, BMP convention)
        // D3D9 surface is top-down, so reverse row order
        unsigned char* src = static_cast<unsigned char*>(pixels);
        double sumLum = 0.0;
        double sumLumSq = 0.0;
        double minLum = 255.0;
        double maxLum = 0.0;
        UINT nearWhiteCount = 0;
        UINT nearBlackCount = 0;
        const UINT totalPixels = width * height;

        for (INT y = static_cast<INT>(height) - 1; y >= 0; y--) {
            unsigned char* rowSrc = src + y * pitch;
            for (UINT x = 0; x < width; x++) {
                unsigned char b = rowSrc[x * 4 + 0];
                unsigned char g = rowSrc[x * 4 + 1];
                unsigned char r = rowSrc[x * 4 + 2];
                unsigned char a = rowSrc[x * 4 + 3];

                // Write BGRA as-is (BMP is BGR order)
                fputc(b, f);
                fputc(g, f);
                fputc(r, f);
                fputc(a, f);

                // Compute luminance (Rec. 709)
                double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                sumLum += lum;
                sumLumSq += lum * lum;
                if (lum < minLum) minLum = lum;
                if (lum > maxLum) maxLum = lum;
                if (lum > 240.0) nearWhiteCount++;
                if (lum < 15.0) nearBlackCount++;
            }
        }

        fclose(f);
        Log("[capture] wrote BMP: %s (%ux%u)\n", bmpPath, width, height);

        // Compute mean and stddev (single-pass via sum-of-squares)
        double meanLum = sumLum / totalPixels;
        double variance = sumLumSq / totalPixels - meanLum * meanLum;
        double stddevLum = sqrt(variance > 0.0 ? variance : 0.0);

        double nearWhiteRatio = static_cast<double>(nearWhiteCount) / totalPixels;
        double nearBlackRatio = static_cast<double>(nearBlackCount) / totalPixels;

        Log("[capture] luminance analysis: mean=%.2f min=%.2f max=%.2f stddev=%.2f\n",
            meanLum, minLum, maxLum, stddevLum);
        Log("[capture] near-white (>240): %.2f%%, near-black (<15): %.2f%%\n",
            nearWhiteRatio * 100.0, nearBlackRatio * 100.0);
    }
};

// ---------------------------------------------------------------------------
// IDirect3D9 wrapper: logs device creation, forwards everything else.
// Only methods the game can call are affected; vtable order is fixed across
// all d3d9 SDK versions (17 methods), so this is ABI-safe.
// ---------------------------------------------------------------------------
class ProxyIDirect3D9 : public IDirect3D9 {
public:
    ProxyIDirect3D9(IDirect3D9* inner, RendererBackend backend,
                    ExplicitOn12Context* explicitOn12Context)
        : m_inner(inner), m_refs(1), m_backend(backend),
          m_explicitOn12Context(explicitOn12Context) {
        Log("[d3d9] IDirect3D9 wrapped (0x%p), backend=%s, adapters=%u\n",
            inner, BackendName(backend), inner ? inner->GetAdapterCount() : 0);
        m_diagConfig = ReadDeviceDiagnosticsConfig();
    }

    ~ProxyIDirect3D9() {
        if (m_explicitOn12Context) {
            Log("[d3d9] releasing explicit D3D12 context after inner IDirect3D9 release\n");
            delete m_explicitOn12Context;
            m_explicitOn12Context = nullptr;
        }
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
    STDMETHOD_(ULONG, AddRef)() {
        return static_cast<ULONG>(InterlockedIncrement(&m_refs));
    }

    STDMETHOD_(ULONG, Release)() {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) {
            IDirect3D9* inner = m_inner;
            m_inner = nullptr;
            if (inner) inner->Release();
            delete this;
        }
        return static_cast<ULONG>(r);
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
        if (!ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;

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

        *ppReturnedDeviceInterface = nullptr;
        IDirect3DDevice9* innerDevice = nullptr;

        // Apply presentation parameter overrides if configured
        D3DPRESENT_PARAMETERS overriddenParams = {};
        D3DPRESENT_PARAMETERS* pParamsToUse = pPresentationParameters;

        if (pPresentationParameters) {
            bool hasOverride = ApplyPresentationOverrides(m_diagConfig, pPresentationParameters, &overriddenParams);
            if (hasOverride) {
                pParamsToUse = &overriddenParams;
            }
        }

        HRESULT hr = m_inner->CreateDevice(Adapter, DeviceType, hFocusWindow,
                                           BehaviorFlags, pParamsToUse,
                                           &innerDevice);

        Log("  -> hr=0x%08x, device=0x%p\n", hr, innerDevice);

        if (SUCCEEDED(hr) && innerDevice) {
            const bool on12DeviceVerified = VerifyDeviceBackend(
                innerDevice, m_backend,
                m_explicitOn12Context ? m_explicitOn12Context->device : nullptr);

            // Log complete presentation parameters after device creation
            // Use the params that were actually passed to CreateDevice
            if (pParamsToUse) {
                Log("[d3d9] Post-CreateDevice PresentationParameters:\n");
                Log("  BackBuffer: %ux%u, Format=%u, Count=%u\n",
                    pParamsToUse->BackBufferWidth,
                    pParamsToUse->BackBufferHeight,
                    pParamsToUse->BackBufferFormat,
                    pParamsToUse->BackBufferCount);
                Log("  MultiSample: Type=%u, Quality=%u\n",
                    pParamsToUse->MultiSampleType,
                    pParamsToUse->MultiSampleQuality);
                Log("  SwapEffect=%u, hDeviceWindow=0x%p, Windowed=%d\n",
                    pParamsToUse->SwapEffect,
                    pParamsToUse->hDeviceWindow,
                    pParamsToUse->Windowed);
                Log("  AutoDepthStencil: Enable=%d, Format=%u\n",
                    pParamsToUse->EnableAutoDepthStencil,
                    pParamsToUse->AutoDepthStencilFormat);
                Log("  Flags=0x%x, FullScreen_RefreshRateInHz=%u, PresentationInterval=0x%x\n",
                    pParamsToUse->Flags,
                    pParamsToUse->FullScreen_RefreshRateInHz,
                    pParamsToUse->PresentationInterval);
                Log("  hFocusWindow=0x%p\n", hFocusWindow);
            }

            // Wrap the device
            ProxyIDirect3DDevice9* proxyDevice = new (std::nothrow) ProxyIDirect3DDevice9(
                innerDevice, this, m_diagConfig, on12DeviceVerified);
            if (!proxyDevice) {
                Log("[d3d9] device wrapper allocation failed\n");
                innerDevice->Release();
                return E_OUTOFMEMORY;
            }
            *ppReturnedDeviceInterface = proxyDevice;
            Log("[d3d9] device wrapped: proxy=0x%p, inner=0x%p\n", proxyDevice, innerDevice);
        }

        return hr;
    }

private:
    IDirect3D9* m_inner;
    volatile LONG m_refs;
    RendererBackend m_backend;
    DeviceDiagnosticsConfig m_diagConfig;
    ExplicitOn12Context* m_explicitOn12Context;
};

// ProxyIDirect3DDevice9 out-of-line implementations (defined after ProxyIDirect3D9)
void ProxyIDirect3DDevice9::AddRefParent() {
    if (m_parent) {
        m_parent->AddRef();
    }
}

void ProxyIDirect3DDevice9::ReleaseParent() {
    if (m_parent) {
        m_parent->Release();
    }
}

HRESULT ProxyIDirect3DDevice9::Intercept_GetDirect3D(IDirect3D9** ppD3D9) {
    if (!ppD3D9) return D3DERR_INVALIDCALL;
    if (m_parent) {
        m_parent->AddRef();
        *ppD3D9 = m_parent;
        return D3D_OK;
    }
    // Fallback if no parent (shouldn't happen)
    return m_inner->GetDirect3D(ppD3D9);
}

// ---------------------------------------------------------------------------
// Direct3DCreate9 hook: select native, DXVK, or 9On12, then wrap the result.
// ---------------------------------------------------------------------------
extern "C" IDirect3D9* WINAPI proxy_Direct3DCreate9(UINT SDKVersion) {
    Log("[d3d9] Direct3DCreate9(SDKVersion=%u)\n", SDKVersion);
    EnsureRealD3D9Loaded();

    RendererBackend requestedBackend = ReadRendererBackend();
    RendererBackend effectiveBackend = requestedBackend;
    IDirect3D9* d = nullptr;
    ExplicitOn12Context* explicitOn12Context = nullptr;

    if (requestedBackend == RendererBackend::Dxvk) {
        EnsureDxvkD3D9Loaded();
        if (!g_DxvkDirect3DCreate9) {
            Log("[dxvk] creation failed: Direct3DCreate9 unavailable (module=%s)\n",
                g_dxvkD3D9Path[0] ? g_dxvkD3D9Path : "<unavailable>");
        } else {
            auto dxvkDirect3DCreate9 =
                reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(g_DxvkDirect3DCreate9);
            d = dxvkDirect3DCreate9(SDKVersion);
            if (d) {
                Log("[dxvk] creation succeeded: module=%s, IDirect3D9=0x%p\n",
                    g_dxvkD3D9Path, d);
            } else {
                Log("[dxvk] creation failed: module=%s, Direct3DCreate9 returned NULL\n",
                    g_dxvkD3D9Path);
            }
        }

        if (!d) {
            effectiveBackend = RendererBackend::Native;
            Log("[d3d9] native fallback selected after DXVK load/create failure\n");
        }
    } else if (requestedBackend == RendererBackend::On12) {
        const On12DeviceMode on12DeviceMode = ReadOn12DeviceMode();
        if (!g_Direct3DCreate9On12) {
            Log("[d3d9] 9On12 creation failed: Direct3DCreate9On12 export unavailable\n");
        } else {
            auto realOn12 = reinterpret_cast<PFN_Direct3DCreate9On12>(g_Direct3DCreate9On12);

            if (on12DeviceMode == On12DeviceMode::Explicit) {
                Log("[d3d9] 9On12 explicit device mode selected\n");
                explicitOn12Context = new (std::nothrow) ExplicitOn12Context();
                if (!explicitOn12Context) {
                    Log("[d3d9] explicit on12 setup failed: context allocation OOM; "
                        "falling back to wildcard 9On12\n");
                } else if (CreateExplicitOn12Context(SDKVersion, explicitOn12Context)) {
                    D3D9ON12_ARGS explicitArgs = {};
                    explicitArgs.Enable9On12 = TRUE;
                    explicitArgs.pD3D12Device = explicitOn12Context->device;
                    explicitArgs.ppD3D12Queues[0] = explicitOn12Context->queue;
                    explicitArgs.NumQueues = 1;
                    explicitArgs.NodeMask = 0;
                    Log("[d3d9] 9On12 creation chosen: SDKVersion=%u, explicit override "
                        "(Enable9On12=TRUE, device=0x%p, queue=0x%p, NumQueues=%u, "
                        "NodeMask=%u)\n",
                        SDKVersion, static_cast<void*>(explicitOn12Context->device),
                        static_cast<void*>(explicitOn12Context->queue), explicitArgs.NumQueues,
                        explicitArgs.NodeMask);
                    d = realOn12(SDKVersion, &explicitArgs, 1);
                    if (d) {
                        Log("[d3d9] explicit 9On12 creation succeeded: IDirect3D9=0x%p\n", d);
                    } else {
                        Log("[d3d9] explicit 9On12 creation failed: returned NULL; "
                            "releasing explicit context and falling back to wildcard 9On12\n");
                        delete explicitOn12Context;
                        explicitOn12Context = nullptr;
                    }
                } else {
                    Log("[d3d9] explicit on12 setup failed; falling back to wildcard 9On12\n");
                    delete explicitOn12Context;
                    explicitOn12Context = nullptr;
                }
            }

            if (!d) {
                D3D9ON12_ARGS args = {};
                args.Enable9On12 = TRUE;
                Log("[d3d9] 9On12 creation chosen: SDKVersion=%u, "
                    "wildcard override (Enable9On12=TRUE, device=0x%p, queues=0x%p, "
                    "NumQueues=%u, NodeMask=%u)\n",
                    SDKVersion, args.pD3D12Device, args.ppD3D12Queues[0],
                    args.NumQueues, args.NodeMask);
                d = realOn12(SDKVersion, &args, 1);
                if (d) {
                    Log("[d3d9] wildcard 9On12 creation succeeded: IDirect3D9=0x%p\n", d);
                } else {
                    Log("[d3d9] wildcard 9On12 creation failed: returned NULL\n");
                }
            }
        }

        if (!d) {
            effectiveBackend = RendererBackend::Native;
            Log("[d3d9] native fallback selected after 9On12 failure\n");
        }
    }

    if (!d) {
        if (!g_Direct3DCreate9) {
            Log("[d3d9] native creation failed: Direct3DCreate9 export unavailable\n");
            return nullptr;
        }
        auto real = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(g_Direct3DCreate9);
        d = real(SDKVersion);
        if (d) {
            Log("[d3d9] native creation succeeded: IDirect3D9=0x%p\n", d);
        }
    }

    if (d) {
        ProxyIDirect3D9* proxy = new (std::nothrow) ProxyIDirect3D9(
            d, effectiveBackend, explicitOn12Context);
        if (!proxy) {
            Log("[d3d9] wrapper allocation failed; releasing inner IDirect3D9 and explicit context\n");
            d->Release();
            delete explicitOn12Context;
            return nullptr;
        }
        return proxy;
    }
    Log("[d3d9] native creation failed: returned NULL\n");
    return nullptr;
}

// ---------------------------------------------------------------------------
BOOLEAN WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    (void)hinstDLL;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

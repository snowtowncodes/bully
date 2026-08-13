<!-- stm:v1 -->
## Session Memory

### User Instructions
- …

### Long Horizon Context
- …

### Decisions
- …

### Conclusions
- …

### Active References
- …

<existing_memory>
## Session Memory

### User Instructions
- None captured yet.

### Long Horizon Context
- None captured yet.

### Decisions
- None captured yet.

### Conclusions
- None captured yet.

### Active References
- None captured yet.

</existing_memory>

<conversation_update>
USER:
Research the technical options for intercepting and translating a legacy Direct3D 9 game's rendering onto DirectX 12 (or Vulkan), in the context of a 2008 RenderWare-based game (Bully: Scholarship Edition PC) that creates its D3D9 device indirectly through DirectDraw (imports DDRAW.dll, then QueryInterfaces to D3D9) and uses d3dx9_38.

Find and report, with URLs and dates:

1. D3D9On12 (Microsoft's official D3D9-to-DX12 translation layer): how to obtain and embed it (does d3d9on12.dll ship with Windows 10/11?), its API surface (Direct3DCreate9On12), how projects use it to redirect an application's D3D9 to DX12 (registry redirect, proxy DLL, DLL search order tricks), known feature gaps and performance characteristics, especially for fixed-function pipeline and old RenderWare-style render state usage. Include official Microsoft docs links and any real-world projects using D3D9On12 as a compatibility layer for old games.

2. Open-source D3D9 translation layers: DXVK / d9vk (D3D9-to-Vulkan), vkd3d, and specifically any D3D9-to-DX12 open-source projects on GitHub. For each: maturity, license, how hard it would be to embed or repurpose for a game-specific wrapper.

3. Proxy-DLL interception patterns for old D3D9 games: how wrappers like ENBSeries, ReShade, dgVoodoo2, d8vk, DDrawCompat hook DirectDrawCreate/Direct3DCreate9, the DLL search-order hijack mechanics, and which functions a RenderWare D3D9 driver would go through (DirectDrawCreate, IDirectDraw7::QueryInterface to IDirect3D9, Direct3DCreate9). Any published analysis of how GTA SA / Bully's rwd3d9 driver initializes the device.

4. Existing open-source "D3D9 to modern API" game-specific wrappers whose code could serve as a reference architecture (e.g. dxwrapper, d9vk, DDrawCompat, cnc-ddraw style projects) - note their license and code structure.

Be concrete: project names, URLs, license names, last-commit dates, and key API/function names I should look up first. If a technology is a dead end (e.g. D3D9On12 known to fail with certain fixed-function usage), say so with evidence. This is research only - do not write any code or files. Return a structured report I can act on.

---

ASSISTANT:
I’ll verify the official D3D9On12 contract first, then cross-check open-source implementations and game-wrapper patterns against repository metadata and published reverse-engineering notes. I’ll distinguish documented facts from inference, and include dates and direct URLs throughout.
</conversation_update>

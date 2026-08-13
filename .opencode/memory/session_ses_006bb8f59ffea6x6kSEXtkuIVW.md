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
What we're doing now (active track)
Building a PC renderer/mod platform for Bully: Scholarship Edition, owned on Steam. Goal: modern rendering (DX12 and similar) and graphics mods on top of the existing native PC game, decompilation if necessary is okay

- you may dump/, recomp/, use tools/, touch the game install if needed.
- you may modify Bully.exe, auto-install a loader.
PS2 track (reference only — do NOT resume)

---

ASSISTANT:
Fresh workspace — only `.opencode/` exists here, so this is day one of the PC track. Before planning, let me establish machine facts: game install location and available toolchains.

---

ASSISTANT:
No Bully in default Steam paths, and SteamPath registry key came back empty. Let me dig deeper — the user owns the game on Steam, so it's on this machine somewhere.

---

ASSISTANT:
Found it — AppID 12200 lives in the E: library. Verifying the install:

---

ASSISTANT:
Game confirmed at `E:\SteamLibrary\steamapps\common\Bully Scholarship Edition`. Interesting — there's already a `dinput8.dll` sitting in there (classic DLL-hijack mod loader spot). Checking the current state of the executable and that DLL:

---

ASSISTANT:
Game located and recon'd. Quick summary of machine facts before I ask the direction question:

**Install state**
- Game: `E:\SteamLibrary\steamapps\common\Bully Scholarship Edition` (~5.3 GB, Steam AppID 12200, v154)
- `dinput8.dll` = ThirteenAG **Ultimate ASI Loader x86** already in place (mod injection point exists)
- `Bully.exe` imports: `DDRAW.dll` (not `d3d9.dll` directly — classic RenderWare PC pattern: device created through DirectDraw, then QI'd to D3D9), `d3dx9_38`, `XINPUT1_3`, `vcomp`
- Engine layout is Rockstar North RW3.x style (same tech family as GTA SA): `Config/timecyc*.dat`, `GRIDS.DAT`, `world.dat`, `MeshLink.dat`, `particle.cfg`, `FxStore.st`, plus `ShaderBinaries/{High,Med,Low,Off}` D3D9 shader bins

**Toolchain here**: Python 3.12.9, git, CMake 4.4, Ninja. No `cl` on PATH (MSVC presence unconfirmed — will check when we need to compile).

I'm kicking off two background research lanes now (ecosystem/decomp landscape + D3D9→DX12 translation tech), since we need those facts regardless of direction:

---

ASSISTANT:
Right — librarian is write-capable, so it goes through `task`. Launching both lanes in parallel:
</conversation_update>

</existing_memory>

<conversation_update>
USER:
sorry for cancelling, i copied the game to the project directory so you could access it without having to access a different drive.

---

ASSISTANT:
No problem — checking the local copy now.

---

ASSISTANT:
Local copy verified — good news, it's a clean original:

- **`Bully.exe` local: 8,204,288 bytes, PE timestamp 2008-12-10** — the original 2008 build (Steam's is a re-signed 8.5 MB variant; same DLL import set: `DDRAW`, `d3dx9_38`, `XINPUT1_3`, `vcomp`)
- Same v154 data (Config/version.cfg), all game dirs present; missing only redist/soundtrack extras. Local copy has no ASI loader yet (Steam install had ThirteenAG's dinput8.dll) — we'll install our own loader when we need it, which you've already pre-approved.

Re-dispatching the two research lanes (they don't touch game files at all, so the drive copy doesn't change them), and meanwhile I need one strategic call from you:
</conversation_update>

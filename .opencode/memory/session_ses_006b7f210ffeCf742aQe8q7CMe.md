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
Research the current modding and decompilation ecosystem for Bully: Scholarship Edition (PC, Steam version, RenderWare-based engine by Rockstar, 2008 port of the 2006 PS2 game).

Context: I am planning a modern rendering/mod platform on top of the PC game. The game already has ThirteenAG's Ultimate ASI Loader (dinput8.dll) installed. Bully.exe imports DDRAW.dll and d3dx9_38 (classic RenderWare d3d9 driver pattern shared with GTA San Andreas PC).

Find and report, with URLs and dates:

1. Existing Bully PC mods/tools, especially graphics-related: widescreen/fps fixes, SilentPatch-style bugfix mods (does SilentPatch exist for Bully?), ENB/ReShade compatibility notes, texture mods, known modding communities (e.g. Bully Board, Steam forums, modding wikis) and their current state.

2. Any PUBLIC decompilation or reimplementation projects for Bully: search GitHub and elsewhere for terms like "bully decomp", "rebully", "openbully", "bully reimplementation", "bully scholarship edition decompilation", "bully reverse engineering". Also check whether any GTA re3-style decomp projects exist for Bully, and their completeness/legal status.

3. RenderWare PC d3d9 driver knowledge: published reverse-engineering notes about how RenderWare games (GTA SA, Bully) create their D3D9 device via DDRAW, the RW d3d9 driver (rwcore, rwd3d9) structure, known hook points, function name databases for Bully.exe or GTA SA.exe (e.g. Ghidra/IDA databases, "gta-reversed", function listings), and how the Bully PC ShaderBinaries format works if anything is published about it.

4. Any existing attempts to replace or upgrade Bully PC's renderer (D3D9 wrappers, Vulkan/DX12 translation, graphics enhancement projects).

Prioritize official sources, GitHub repos, and long-lived community forums over dead links. Report concrete project names, URLs, last-activity dates, and license status. If something does not exist, say so explicitly rather than padding. This is research only - do not write any code or files. Return a structured report I can act on.

---

ASSISTANT:
I’ll split the research across the three evidence areas: Bully mod ecosystem, public decompilation/reimplementation projects, and RenderWare/renderer internals. I’ll cross-check findings against primary project pages or repository metadata and distinguish active projects from abandoned or speculative references.
</conversation_update>

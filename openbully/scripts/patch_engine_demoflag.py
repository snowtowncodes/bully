# One-shot patch 5: --demo-only flag loads just the three verified test meshes.
import os

path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "src", "engine", "engine.cpp")
path = os.path.normpath(path)
src = open(path, encoding="utf-8").read()

# Engine gets a demoOnly flag member via Init-time static: simplest is an
# env check — zero header churn, self-documenting log line.
old = """    // World.img level geometry: NIFs extracted to data/extracted/world next
    // to the executable (deployed by the build) or the source-tree fallback.
    // Loaded AFTER the renderer so GPU handles are valid; capped for the
    // Phase-3 milestone (full streaming comes later).
    size_t worldLoaded = LoadWorldGeometry("data/extracted/world", 32);
    if (worldLoaded == 0) {
        // Fallback: source-tree relative path (running from a dev shell).
        worldLoaded = LoadWorldGeometry("../data/extracted/world", 32);
    }"""
new = """    // World.img level geometry: NIFs extracted to data/extracted/world next
    // to the executable (deployed by the build) or the source-tree fallback.
    // Loaded AFTER the renderer so GPU handles are valid; capped for the
    // Phase-3 milestone (full streaming comes later).
    // OPENBULLY_DEMO_ONLY=1 restricts the scene to the verified demo trio
    // (JimmysRoom01 + 3_06CarBarr01 + 1950Fridge) for focused screenshots.
    const bool demoOnly = std::getenv("OPENBULLY_DEMO_ONLY") != nullptr;
    const size_t meshCap = demoOnly ? 3 : 32;
    size_t worldLoaded = LoadWorldGeometry("data/extracted/world", meshCap);
    if (worldLoaded == 0) {
        // Fallback: source-tree relative path (running from a dev shell).
        worldLoaded = LoadWorldGeometry("../data/extracted/world", meshCap);
    }
    if (demoOnly) Logger::Info("Demo-only mode: restricted to the 3 test meshes");"""
if old in src:
    src = src.replace(old, new, 1)
    if "#include <cstdlib>" not in src:
        src = src.replace("#include <cmath>", "#include <cmath>\n#include <cstdlib>", 1)
    open(path, "w", encoding="utf-8", newline="\n").write(src)
    print("engine.cpp demo-only patched")
elif "OPENBULLY_DEMO_ONLY" in src:
    print("already patched")
else:
    raise SystemExit("anchor not found")

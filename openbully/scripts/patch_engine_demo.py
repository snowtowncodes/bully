# One-shot patch 4: prioritize the verified demo meshes in LoadWorldGeometry.
import os

path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "src", "engine", "engine.cpp")
path = os.path.normpath(path)
src = open(path, encoding="utf-8").read()

old = """    ::closedir(dp);
    std::sort(names.begin(), names.end());
"""
new = """    ::closedir(dp);

    // Deterministic order, demo set first: the verified test meshes named in
    // the task spec (room interior + car barrier + 1950 fridge) render as a
    // coherent little scene; the remaining slots fill alphabetically.
    static const char* kDemoMeshes[] = {
        "JimmysRoom01.nif",
        "3_06CarBarr01.nif",
        "1950Fridge.nif",
    };
    auto rank = [](const std::string& n) {
        for (size_t i = 0; i < sizeof(kDemoMeshes) / sizeof(kDemoMeshes[0]); i++)
            if (n == kDemoMeshes[i]) return static_cast<int>(i);
        return 1000;
    };
    std::sort(names.begin(), names.end(), [&rank](const std::string& a, const std::string& b) {
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        return a < b;
    });
"""
if old in src:
    src = src.replace(old, new, 1)
    open(path, "w", encoding="utf-8", newline="\n").write(src)
    print("engine.cpp demo-priority patched")
elif "kDemoMeshes" in src:
    print("already patched")
else:
    raise SystemExit("anchor not found")

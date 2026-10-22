# One-shot patch 2: append NifWorldTransform + parent-link post-pass to nif_loader.cpp.
import os

path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "src", "engine", "formats", "nif_loader.cpp")
path = os.path.normpath(path)
src = open(path, encoding="utf-8").read()

changed = False

# 1) Parent-link post-pass: insert right before 'result.ok = !result.geometries.empty();'
if "parent = static_cast<int32_t>(c);" not in src:
    anchor = "    result.ok = !result.geometries.empty();"
    pos = src.find(anchor)
    assert pos >= 0, "result.ok anchor not found"
    postpass = (
        "    // Fill parent links from the children refs (NIF stores one side only).\n"
        "    for (uint32_t n = 0; n < result.nodes.size(); n++)\n"
        "        for (uint32_t c : result.nodes[n].children)\n"
        "            if (c < result.nodes.size() &&\n"
        "                result.nodes[c].parent < 0)\n"
        "                result.nodes[c].parent = static_cast<int32_t>(n);\n\n"
    )
    src = src[:pos] + postpass + src[pos:]
    changed = True

# 2) Append NifWorldTransform before the namespace closer.
if "NifWorldTransform" not in src:
    marker = "} // namespace OpenBully"
    idx = src.rfind(marker)
    assert idx >= 0, "namespace closer not found"
    addition = """bool NifWorldTransform(const NifLoadResult& nif, uint32_t index, float out[16]) {
    if (index >= nif.nodes.size()) return false;

    // Local transform: T * R * S, column-major 4x4 (bgfx convention).
    // NIF stores the 3x3 rotation row-major; R_col[c*4+r] = rotation[r*3+c].
    auto local = [](const NifNode& n, float m[16]) {
        for (int k = 0; k < 16; k++) m[k] = 0.0f;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                m[c * 4 + r] = n.rotation[r * 3 + c] * n.scale;
        m[12] = n.translation.x;
        m[13] = n.translation.y;
        m[14] = n.translation.z;
        m[15] = 1.0f;
    };

    auto mul = [](const float* a, const float* b, float* o) { // o = a * b
        float r[16];
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++)
                    s += a[k * 4 + row] * b[c * 4 + k];
                r[c * 4 + row] = s;
            }
        for (int k = 0; k < 16; k++) o[k] = r[k];
    };

    float acc[16];
    local(nif.nodes[index], acc);

    // Compose child-to-root up the parent chain (cycle-capped defensively).
    int32_t parent = nif.nodes[index].parent;
    int depth = 0;
    while (parent >= 0 &&
           static_cast<uint32_t>(parent) < nif.nodes.size() && depth < 64) {
        float pm[16];
        local(nif.nodes[parent], pm);
        float res[16];
        mul(pm, acc, res);
        for (int k = 0; k < 16; k++) acc[k] = res[k];
        parent = nif.nodes[parent].parent;
        ++depth;
    }

    for (int k = 0; k < 16; k++) out[k] = acc[k];
    return true;
}

"""
    src = src[:idx] + addition + src[idx:]
    changed = True

if changed:
    open(path, "w", encoding="utf-8", newline="\n").write(src)
    print("patched: parent links + NifWorldTransform")
else:
    print("nothing to do")

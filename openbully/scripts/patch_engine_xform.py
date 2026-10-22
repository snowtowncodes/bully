# One-shot patch 3: engine.cpp bakes NIF world transforms into vertices.
import os

path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "src", "engine", "engine.cpp")
path = os.path.normpath(path)
src = open(path, encoding="utf-8").read()

old = """        Mesh mesh;
        if (!MeshFromNif(nif, mesh)) {
            failed++;
            continue;
        }
"""
new = """        Mesh mesh;
        if (!MeshFromNif(nif, mesh)) {
            failed++;
            continue;
        }

        // Bake each geometry's world transform (NiNode hierarchy) into the
        // vertices: positions move through the full chain, normals through
        // rotation only (uniform scale assumed for level geometry).
        // MeshFromNif concatenates geometries in order; recompute the vertex
        // ranges per geometry from the sub-mesh list.
        {
            size_t vertBase = 0;
            size_t geomIdx = 0;
            for (const NifGeometryData& geo : nif.geometries) {
                float wm[16];
                if (NifWorldTransform(nif, geo.nodeIndex, wm)) {
                    for (size_t v = 0; v < geo.vertices.size(); v++) {
                        MeshVertex& mv = mesh.vertices[vertBase + v];
                        const NifVector3& p = geo.vertices[v];
                        const float x = wm[0]*p.x + wm[4]*p.y + wm[8]*p.z + wm[12];
                        const float y = wm[1]*p.x + wm[5]*p.y + wm[9]*p.z + wm[13];
                        const float z = wm[2]*p.x + wm[6]*p.y + wm[10]*p.z + wm[14];
                        mv.x = x; mv.y = y; mv.z = z;
                        const NifVector3& nrm = geo.normals[v];
                        const float nx = wm[0]*nrm.x + wm[4]*nrm.y + wm[8]*nrm.z;
                        const float ny = wm[1]*nrm.x + wm[5]*nrm.y + wm[9]*nrm.z;
                        const float nz = wm[2]*nrm.x + wm[6]*nrm.y + wm[10]*nrm.z;
                        mv.nx = nx; mv.ny = ny; mv.nz = nz;
                    }
                }
                vertBase += geo.vertices.size();
                ++geomIdx;
            }
        }
"""
if old in src:
    src = src.replace(old, new, 1)
    open(path, "w", encoding="utf-8", newline="\n").write(src)
    print("engine.cpp patched")
elif "Bake each geometry" in src:
    print("already patched")
else:
    raise SystemExit("anchor not found")

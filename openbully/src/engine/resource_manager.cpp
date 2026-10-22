#include "resource_manager.h"
#include "formats/nif_loader.h"
#include "../platform/logger.h"

namespace OpenBully {

ResourceManager& ResourceManager::Get() {
    static ResourceManager instance;
    return instance;
}

ResourceHandle ResourceManager::Register(const std::string& path, ResourceType type) {
    // Duplicate check
    for (const auto& [id, info] : m_resources) {
        if (info.path == path && info.type == type) {
            Logger::Warning("Resource already registered: %s", path.c_str());
            return static_cast<ResourceHandle>(id);
        }
    }

    const unsigned int id = m_nextId++;
    m_resources.emplace(id, ResourceInfo{path, type, 0});
    return static_cast<ResourceHandle>(id);
}

void* ResourceManager::Acquire(ResourceHandle handle) {
    const unsigned int id = static_cast<unsigned int>(handle);
    auto it = m_resources.find(id);
    if (it == m_resources.end()) {
        Logger::Warning("Acquire on unknown resource handle %u", id);
        return nullptr;
    }
    // Phase 1 stub: no actual loading yet.
    return nullptr;
}

bool ResourceManager::LoadMesh(const std::string& path, Mesh& outMesh) {
    const NifLoadResult nif = LoadNifFile(path);
    if (!nif.ok) {
        Logger::Error("NIF load failed for %s: %s", path.c_str(),
                      nif.error.c_str());
        return false;
    }
    if (!MeshFromNif(nif, outMesh)) {
        Logger::Error("NIF produced an empty mesh for %s", path.c_str());
        return false;
    }
    Logger::Info("Loaded mesh %s: %zu verts, %zu tris, %zu sub-meshes",
                 path.c_str(), outMesh.VertexCount(), outMesh.TriangleCount(),
                 outMesh.subMeshes.size());
    return true;
}

void ResourceManager::Clear() {
    m_resources.clear();
    m_nextId = 1;
}

} // namespace OpenBully

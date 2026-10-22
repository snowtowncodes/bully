#pragma once

// OpenBully: Resource manager (Phase 1 stub).
// Track 1 deliverable: registry skeleton with typed handles and path mapping.
// Actual loading (TXD/TXD textures, DFF models, audio streams) lands in later
// tracks; for now resources are "registered" and counted, never loaded.

#include <string>
#include <unordered_map>
#include <memory>

#include "../rendering/mesh.h"

namespace OpenBully {

// Opaque typed resource handle.
enum class ResourceHandle : unsigned int { Invalid = 0 };

enum class ResourceType {
    Texture,
    Model,
    Audio,
    Shader,
    Font,
};

struct ResourceInfo {
    std::string path;
    ResourceType type;
    // Placeholder for future load state: NotLoaded / Loading / Ready / Failed
    int state = 0;
};

class ResourceManager {
public:
    static ResourceManager& Get();

    // Register a resource path for future loading. Returns a handle, or
    // Invalid on duplicates (same path + type).
    ResourceHandle Register(const std::string& path, ResourceType type);

    // Phase 1 stub: always returns nullptr. Later tracks return the loaded
    // resource payload.
    void* Acquire(ResourceHandle handle);

    // Track 3: load a .nif file into a GPU-uploadable Mesh. Returns false and
    // logs on failure. The mesh is cached per handle.
    bool LoadMesh(const std::string& path, Mesh& outMesh);

    size_t GetRegisteredCount() const { return m_resources.size(); }

    // Clear all registrations (used on engine shutdown / asset reload).
    void Clear();

private:
    ResourceManager() = default;

    std::unordered_map<unsigned int, ResourceInfo> m_resources;
    unsigned int m_nextId = 1;
};

} // namespace OpenBully

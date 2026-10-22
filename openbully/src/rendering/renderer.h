#pragma once

// Renderer: bgfx-backed rendering front-end.
// Owns the bgfx lifecycle (init / frame / shutdown), shader programs and
// GPU buffer uploads for Mesh instances (see mesh.h - Track 3 layout).

#include "../rendering/mesh.h"

#include <bgfx/bgfx.h>
#include <cstdint>
#include <vector>

namespace OpenBully {

// A mesh uploaded to the GPU: vertex/index buffer handles plus draw metadata.
// Returned by UploadMeshGpu; the engine keeps these alive until Shutdown.
struct GpuMesh {
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  ibh = BGFX_INVALID_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    bool valid() const { return bgfx::isValid(vbh) && bgfx::isValid(ibh); }
};

// Renderer: bgfx-backed rendering front-end (see file header).
class Renderer {
public:
    Renderer() = default;
    ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Initializes bgfx against a native OS window (HWND on Win32).
    // `shaderRoot` is a directory containing per-backend subdirectories
    // (direct3d11/, opengl/, vulkan/) with compiled shader binaries produced
    // by the build (shaderc invocations in CMakeLists.txt).
    bool Init(void* windowHandle, uint32_t width, uint32_t height, const char* shaderRoot);
    void Shutdown();

    // Presents the frame. Call exactly once per game frame, after all draws.
    void Frame();

    // Uploads a CPU mesh (Track 3 Mesh) to GPU vertex/index buffers. Phase 2
    // keeps a single uploaded mesh; re-upload replaces it.
    bool UploadMesh(const Mesh& mesh);

    // Draws `indexCount` indices starting at `indexOffset` with a clip-space
    // transform: position' = position * scale.xy + offset.xy (NDC).
    void DrawMesh(uint32_t indexOffset, uint32_t indexCount,
                  float offsetX, float offsetY, float scaleX, float scaleY);

    // --- 3D geometry path (World.img NIFs) ----------------------------------

    // Uploads an arbitrary number of meshes, each as an independent GPU
    // buffer pair. Unlike UploadMesh (single-mesh Phase-2 cache), this does
    // not replace previous uploads and does not enforce the 16-bit vertex
    // cap (32-bit indices are used throughout).
    GpuMesh UploadMeshGpu(const Mesh& mesh);

    // Draws a 3D mesh with a full model-view-projection matrix. Geometry is
    // expected in Gamebryo world units; the shader receives the MVP via the
    // u_mvp mat4 uniform.
    void DrawMesh3D(const GpuMesh& mesh, const float mvp[16]);

    // Sets the per-frame camera (eye, target, up) used by the next frame's
    // 3D view setup. Call once per frame before DrawMesh3D calls.
    void SetCamera(const float eye[3], const float target[3], const float up[3]);

    // Destroys a previously uploaded GpuMesh's buffers.
    void DestroyGpuMesh(GpuMesh& mesh);

    bool IsInitialized() const { return m_initialized; }

    // Shared bgfx vertex layout for MeshVertex (see mesh.h).
    static const bgfx::VertexLayout& GetVertexLayout();

private:
    bool LoadShaders(const char* shaderRoot);

    bgfx::VertexBufferHandle m_vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  m_ibh = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle      m_prog = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      m_uTransform = BGFX_INVALID_HANDLE;

    // 3D path
    bgfx::ProgramHandle m_prog3d = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uMvp = BGFX_INVALID_HANDLE;
    float m_eye[3]    = {0.0f, 0.0f, 0.0f};
    float m_target[3] = {0.0f, 1.0f, 0.0f};
    float m_up[3]     = {0.0f, 0.0f, 1.0f};

    uint32_t m_windowWidth  = 0;
    uint32_t m_windowHeight = 0;

    bool m_initialized = false;
};

} // namespace OpenBully

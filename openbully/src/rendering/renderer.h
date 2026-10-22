#pragma once

// Renderer: bgfx-backed rendering front-end.
// Owns the bgfx lifecycle (init / frame / shutdown), shader programs and
// GPU buffer uploads for Mesh instances (see mesh.h - Track 3 layout).

#include "../rendering/mesh.h"
#include "../engine/formats/nft_loader.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <bgfx/bgfx.h>
#include <cstdint>
#include <string>
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

// A texture uploaded to the GPU from a decoded NFT (Track 11).
struct GpuTexture {
    bgfx::TextureHandle th = BGFX_INVALID_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid() const { return bgfx::isValid(th); }
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

    // --- Textures (Track 11, NFT loader) -----------------------------------

    // Decodes an .nft file and uploads it as an RGBA8 texture. When
    // `wantName` is non-empty, decodes the matching NiSourceTexture inside
    // the dictionary; empty decodes the first texture in the file. Returns
    // an invalid handle on failure (caller should fall back to placeholder).
    GpuTexture LoadTexture(const std::string& nftPath,
                           const std::string& wantName = {});

    // Uploads an already-decoded NftTexture (e.g. the placeholder checker).
    GpuTexture UploadTexture(const NftTexture& tex);

    // Binds `texture` for subsequent DrawMesh3D(Sub) calls until the next
    // BindTexture. Invalid handle binds the built-in placeholder.
    void BindTexture(const GpuTexture& texture);
    // Destroys a previously uploaded GpuTexture.
    void DestroyTexture(GpuTexture& texture);

    // --- 3D geometry path (World.img NIFs) ----------------------------------

    // Uploads an arbitrary number of meshes, each as an independent GPU
    // buffer pair. Unlike UploadMesh (single-mesh Phase-2 cache), this does
    // not replace previous uploads and does not enforce the 16-bit vertex
    // cap (32-bit indices are used throughout).
    GpuMesh UploadMeshGpu(const Mesh& mesh);

    // Draws a 3D mesh with a full model-view-projection matrix. Geometry is
    // expected in Gamebryo world units; the shader receives the MVP via the
    // u_mvp mat4 uniform. Samples the texture bound via BindTexture.
    void DrawMesh3D(const GpuMesh& mesh, const float mvp[16]);

    // Draws an index range of a 3D mesh (per-submesh texture binding).
    void DrawMesh3DSub(const GpuMesh& mesh, uint32_t indexOffset,
                       uint32_t indexCount, const float mvp[16]);

    // --- Skinned path (Track 17) --------------------------------------------

    // Shared bgfx vertex layout for SkinnedMeshVertex (pos/normal/uv + 4
    // bone indices + 4 weights).
    static const bgfx::VertexLayout& GetSkinnedVertexLayout();

    // Uploads a skinned CPU mesh (SkinnedMesh) to GPU vertex/index buffers
    // (32-bit indices, own buffer pair like UploadMeshGpu).
    GpuMesh UploadSkinnedMeshGpu(const SkinnedMesh& mesh);

    // Draws an index range of a skinned mesh, transforming every vertex by
    // the weighted sum of `boneCount` column-major 4x4 skin matrices (the
    // AnimationController's SkinMatrices()). Unused palette slots are filled
    // with identity on the renderer side.
    void DrawMesh3DSubSkinned(const GpuMesh& mesh, uint32_t indexOffset,
                              uint32_t indexCount, const float mvp[16],
                              const float* boneMatrices, uint32_t boneCount);

    // --- Debug wireframe boxes (Track 13, OPENBULLY_DEBUG_COLLISION) --------

    // Draws a unit wireframe cube transformed by `model` (untextured, solid
    // line-ish color comes from the flat 3D shader with a white texture).
    // Call once per collision box per frame; sets up view 2 on first use.
    void DrawDebugBox(const float model[16]);

    // Column-major projection matrix for the 3D view (fov/aspect/near/far
    // owned here). Callers multiply: mvp = proj * view * model.
    const glm::mat4& GetProjection() const { return m_projection; }

    // Debug/verification: force the 3D view's projection to identity (used
    // with OPENBULLY_DEBUG_CAM=1 to take the camera out of the pipeline).
    void DebugIdentityProjection() { m_projection = glm::mat4(1.0f); }

    // Sets the per-frame camera used by the next frame's 3D view: a view
    // matrix supplied by the camera (Track 10) instead of eye/target/up.
    void SetCameraView(const glm::mat4& view);

    // Configures the single directional light used by the 3D path (Track 12).
    // `dir` points from the scene toward the light source ("to-light");
    // color/ambient are RGB tints (0..1).
    void SetLight(const float dir[3], const float color[3], const float ambient[3]);

    // Asks bgfx to dump the backbuffer to a file next to the executable on an
    // upcoming frame (default callback appends .tga). Verification hook for
    // lighting work; harmless in normal play.
    void RequestScreenshot(const char* name);

    // Builds the shared unit-cube wireframe mesh for DrawDebugBox. Returns
    // false when GPU upload fails (debug boxes silently skip).
    bool InitDebugBox();

    // Destroys a previously uploaded GpuMesh's buffers.
    void DestroyGpuMesh(GpuMesh& mesh);

    bool IsInitialized() const { return m_initialized; }

    // Shared bgfx vertex layout for MeshVertex (see mesh.h).
    static const bgfx::VertexLayout& GetVertexLayout();

private:
    bool LoadShaders(const char* shaderRoot);

    // Shared 3D submit path (view setup + texture bind + state + submit).
    void DrawMesh3DRange(const GpuMesh& mesh, uint32_t indexOffset,
                         uint32_t indexCount, const float mvp[16]);

    bgfx::VertexBufferHandle m_vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  m_ibh = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle      m_prog = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle      m_uTransform = BGFX_INVALID_HANDLE;

    // 3D path
    bgfx::ProgramHandle m_prog3d = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uMvp = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sTex = BGFX_INVALID_HANDLE;   // s_matBase sampler

    // Track 17: skinned variant + bone-matrix palette uniform.
    bgfx::ProgramHandle m_prog3dSkinned = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uBones = BGFX_INVALID_HANDLE;
    std::vector<float> m_bonePalette;   // kMaxSkinBones * 16 floats
    bool m_pendingBoneUniform = false;

    // Directional light uniforms (Track 12). bgfx has no vec3 uniform type,
    // so each light vector is packed into a vec4 (only .xyz is read).
    bgfx::UniformHandle m_uLightDir     = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLightColor   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uAmbientColor = BGFX_INVALID_HANDLE;
    // Shader-side light travel direction: negation of the to-light dir set
    // via SetLight (which points from the scene toward the sun).
    float m_lightDir[3]     = { -0.3f, 0.8f, 0.5f };  // default sun from +Y above
    float m_lightColor[3]   = { 1.0f, 1.0f, 1.0f };   // white sun
    float m_ambientColor[3] = { 0.2f, 0.2f, 0.2f };   // 0.2 gray ambient floor

    // Track 12 verification hook: one backbuffer capture requested at a time.
    bool        m_screenshotRequested = false;
    std::string m_screenshotName;

    // Track 13 debug wireframe box (unit cube + program shared with the 3D
    // path, drawn with a magenta-ish tint via the light uniforms override).
    GpuMesh m_debugBox;
    bool    m_debugBoxValid = false;
    GpuTexture m_placeholderTex;                        // magenta/black 4x4
    GpuTexture m_boundTex;                              // last BindTexture
    glm::mat4 m_projection{1.0f};
    glm::mat4 m_view{1.0f};

    uint32_t m_windowWidth  = 0;
    uint32_t m_windowHeight = 0;

    bool m_initialized = false;
};

} // namespace OpenBully

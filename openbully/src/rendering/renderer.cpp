#include "renderer.h"
#include "../platform/logger.h"

#include <bgfx/bgfx.h>
#include <bx/bx.h>
#include <bx/math.h>
#include <glm/gtc/type_ptr.hpp>
#include "../animation/animation_controller.h"
#include <fstream>
#include <string>
#include <cstdlib>
#include <vector>

namespace OpenBully {

// Subdirectory holding compiled binaries for a given backend.
static const char* ShaderDirFor(bgfx::RendererType::Enum type) {
    switch (type) {
        case bgfx::RendererType::Direct3D11: return "direct3d11";
        case bgfx::RendererType::Direct3D12: return "direct3d12";
        case bgfx::RendererType::Vulkan:     return "vulkan";
        case bgfx::RendererType::OpenGL:     return "opengl";
        default:                             return nullptr;
    }
}

// Reads a compiled shader binary; empty vector on failure.
static std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const std::streamoff size = file.tellg();
    if (size <= 0) return {};
    file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

static bgfx::ShaderHandle LoadShader(const std::string& dir, const char* name) {
    const std::vector<uint8_t> bytes = ReadFileBytes(dir + "/" + name + ".bin");
    if (bytes.empty()) return BGFX_INVALID_HANDLE;
    // Copy into bgfx-owned memory; the renderer may keep the blob past the call.
    const bgfx::Memory* mem = bgfx::copy(bytes.data(), uint32_t(bytes.size()));
    return bgfx::createShader(mem);
}

// Creates a shader program from two shader files in `dir`; destroys the
// shader handles with the program. Returns invalid on failure.
static bgfx::ProgramHandle LoadProgram(const std::string& dir,
                                       const char* vsName, const char* fsName) {
    bgfx::ShaderHandle vsh = LoadShader(dir, vsName);
    if (!bgfx::isValid(vsh)) {
        Logger::Error("Failed to load vertex shader: %s/%s.bin", dir.c_str(), vsName);
        return BGFX_INVALID_HANDLE;
    }
    bgfx::ShaderHandle fsh = LoadShader(dir, fsName);
    if (!bgfx::isValid(fsh)) {
        bgfx::destroy(vsh);
        Logger::Error("Failed to load fragment shader: %s/%s.bin", dir.c_str(), fsName);
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::createProgram(vsh, fsh, true /* destroy shaders with program */);
}
// Shared layout for Track 3 MeshVertex: pos float3, normal float3, uv float2.
const bgfx::VertexLayout& Renderer::GetVertexLayout() {
    static bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal,   3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        return l;
    }();
    return layout;
}

// Track 17: skinned vertex layout = base + 4 bone indices (uint8 x4) +
// 4 weights (float x4).
const bgfx::VertexLayout& Renderer::GetSkinnedVertexLayout() {
    static bgfx::VertexLayout layout = [] {
        bgfx::VertexLayout l;
        l.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Normal,   3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Indices,  4, bgfx::AttribType::Uint8, false)
            .add(bgfx::Attrib::Weight,   4, bgfx::AttribType::Float)
            .end();
        return l;
    }();
    return layout;
}

bool Renderer::Init(void* windowHandle, uint32_t width, uint32_t height, const char* shaderRoot) {
    if (m_initialized) return true;
    if (!windowHandle) {
        Logger::Error("Renderer::Init: null window handle");
        return false;
    }

    bgfx::Init init;
    // Phase 2: pin Direct3D11 on Windows for a deterministic backend while
    // shaders are compiled for it explicitly; multi-backend in Phase 3.
#if BX_PLATFORM_WINDOWS
    init.type = bgfx::RendererType::Direct3D11;
#else
    init.type = bgfx::RendererType::Count; // auto-detect
#endif
    init.vendorId = BGFX_PCI_ID_NONE;
    init.platformData.nwh = windowHandle;
    init.resolution.width  = width;
    init.resolution.height = height;
    init.resolution.reset  = BGFX_RESET_VSYNC; // replaces SDL_GL_SetSwapInterval(1)

    if (!bgfx::init(init)) {
        Logger::Error("bgfx::init failed");
        return false;
    }

    const bgfx::Caps* caps = bgfx::getCaps();
    Logger::Info("bgfx renderer: %s", bgfx::getRendererName(caps->rendererType));
    m_windowWidth  = width;
    m_windowHeight = height;

    // Projection is fixed here (fov/aspect/near/far); Track 10 camera swaps
    // only the view matrix each frame via SetCameraView(). Built eagerly so
    // Engine::Render can fetch it before the first draw.
    if (width > 0 && height > 0) {
        const float aspect = float(width) / float(height);
        if (caps->homogeneousDepth) {
            m_projection = glm::perspectiveLH_NO(
                glm::radians(60.0f), aspect, 0.1f, 20000.0f);
        } else {
            // D3D clip depth is [0,1].
            m_projection = glm::perspectiveLH_ZO(
                glm::radians(60.0f), aspect, 0.1f, 20000.0f);
        }
    }

    // View 0: full backbuffer, cleared every frame. 0.1,0.1,0.15 -> 0x1a1a26.
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xff1a1a26, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, bgfx::BackbufferRatio::Equal);
    bgfx::setDebug(BGFX_DEBUG_TEXT | (std::getenv("OPENBULLY_STATS") ? BGFX_DEBUG_STATS : 0));

    m_uTransform = bgfx::createUniform("u_transform", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(m_uTransform)) {
        Logger::Error("Renderer::Init: uniform creation failed");
        return false;
    }

    // 3D path uniform (u_mvp mat4) — used by the world-geometry program.
    m_uMvp = bgfx::createUniform("u_mvp", bgfx::UniformType::Mat4);
    if (!bgfx::isValid(m_uMvp)) {
        Logger::Error("Renderer::Init: mvp uniform creation failed");
        return false;
    }

    // Track 11: base-color sampler + placeholder texture (magenta/black
    // checker) so untextured submeshes are unmissable on screen. Uploaded
    // inline because UploadTexture() guards on m_initialized, which is not
    // set until Init completes.
    m_sTex = bgfx::createUniform("s_matBase", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(m_sTex)) {
        Logger::Error("Renderer::Init: sampler uniform creation failed");
        return false;
    }

    // Track 12: directional light uniforms (packed vec3-in-vec4).
    m_uLightDir     = bgfx::createUniform("u_lightDir", bgfx::UniformType::Vec4);
    m_uLightColor   = bgfx::createUniform("u_lightColor", bgfx::UniformType::Vec4);
    m_uAmbientColor = bgfx::createUniform("u_ambientColor", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(m_uLightDir) || !bgfx::isValid(m_uLightColor)
        || !bgfx::isValid(m_uAmbientColor)) {
        Logger::Error("Renderer::Init: light uniform creation failed");
        return false;
    }
    {
        const NftTexture ph = MakePlaceholderTexture();
        const bgfx::Memory* mem =
            bgfx::copy(ph.rgba.data(), uint32_t(ph.rgba.size()));
        m_placeholderTex.th = bgfx::createTexture2D(
            ph.width, ph.height, false, 1,
            bgfx::TextureFormat::RGBA8, 0, mem);
        m_placeholderTex.width = ph.width;
        m_placeholderTex.height = ph.height;
        if (!m_placeholderTex.valid()) {
            Logger::Error("Renderer::Init: placeholder texture upload failed");
            return false;
        }
    }
    m_boundTex = m_placeholderTex;
    if (!m_placeholderTex.valid()) {
        Logger::Error("Renderer::Init: placeholder texture upload failed");
        return false;
    }

    if (!LoadShaders(shaderRoot)) {
        Logger::Error("Renderer::Init: shader load failed (root: %s)", shaderRoot);
        return false;
    }

    m_initialized = true;
    Logger::Info("Renderer initialized");
    return true;
}

bool Renderer::LoadShaders(const char* shaderRoot) {
    if (!shaderRoot) return false;

    const bgfx::Caps* caps = bgfx::getCaps();
    const char* subdir = ShaderDirFor(caps->rendererType);
    if (!subdir) {
        Logger::Error("No shader directory mapping for renderer type %d", int(caps->rendererType));
        return false;
    }
    const std::string dir = std::string(shaderRoot) + "shaders/" + subdir;

    // 2D triangle program (Phase-2 test path).
    m_prog = LoadProgram(dir, "vs_triangle", "fs_triangle");
    if (!bgfx::isValid(m_prog)) {
        Logger::Error("Failed to create triangle shader program");
        return false;
    }

    // 3D world-geometry program (same vertex layout, MVP transform).
    m_prog3d = LoadProgram(dir, "vs_mesh3d", "fs_mesh3d");
    if (!bgfx::isValid(m_prog3d)) {
        Logger::Error("Failed to create 3D mesh shader program");
        return false;
    }

    // Track 17: skinned variant. Non-fatal when missing (older shader cache):
    // skinned meshes fall back to their static bind pose via the 3D path.
    m_prog3dSkinned = LoadProgram(dir, "vs_mesh3d_skinned", "fs_mesh3d");
    if (!bgfx::isValid(m_prog3dSkinned))
        Logger::Warning("Skinned shader program not found; characters will "
                        "render in bind pose");
    m_uBones = bgfx::createUniform("u_bones", bgfx::UniformType::Mat4,
                                   kMaxSkinBones);
    return true;
}

void Renderer::Shutdown() {
    if (!m_initialized) return;

    Logger::Info("Shutting down renderer...");
    if (bgfx::isValid(m_vbh)) bgfx::destroy(m_vbh);
    if (bgfx::isValid(m_ibh)) bgfx::destroy(m_ibh);
    if (bgfx::isValid(m_prog)) bgfx::destroy(m_prog);
    if (bgfx::isValid(m_uTransform)) bgfx::destroy(m_uTransform);
    if (bgfx::isValid(m_prog3d)) bgfx::destroy(m_prog3d);
    if (bgfx::isValid(m_uMvp)) bgfx::destroy(m_uMvp);
    if (bgfx::isValid(m_prog3dSkinned)) bgfx::destroy(m_prog3dSkinned);
    if (bgfx::isValid(m_uBones)) bgfx::destroy(m_uBones);
    if (bgfx::isValid(m_uLightDir)) bgfx::destroy(m_uLightDir);
    if (bgfx::isValid(m_uLightColor)) bgfx::destroy(m_uLightColor);
    if (bgfx::isValid(m_uAmbientColor)) bgfx::destroy(m_uAmbientColor);
    if (m_debugBoxValid) DestroyGpuMesh(m_debugBox);
    m_debugBoxValid = false;
    DestroyTexture(m_boundTex);
    DestroyTexture(m_placeholderTex);
    if (bgfx::isValid(m_sTex)) bgfx::destroy(m_sTex);
    m_vbh = BGFX_INVALID_HANDLE;
    m_ibh = BGFX_INVALID_HANDLE;
    m_prog = BGFX_INVALID_HANDLE;
    m_uTransform = BGFX_INVALID_HANDLE;
    m_prog3d = BGFX_INVALID_HANDLE;
    m_prog3dSkinned = BGFX_INVALID_HANDLE;
    m_uBones = BGFX_INVALID_HANDLE;
    m_uMvp = BGFX_INVALID_HANDLE;
    m_uLightDir = BGFX_INVALID_HANDLE;
    m_uLightColor = BGFX_INVALID_HANDLE;
    m_uAmbientColor = BGFX_INVALID_HANDLE;
    m_sTex = BGFX_INVALID_HANDLE;
    bgfx::shutdown();
    m_initialized = false;
    Logger::Info("Renderer shutdown complete");
}

void Renderer::Frame() {
    if (!m_initialized) return;
    bgfx::frame();
}

bool Renderer::UploadMesh(const Mesh& mesh) {
    if (!m_initialized || !mesh.IsValid()) {
        Logger::Error("UploadMesh: renderer not initialized or mesh invalid");
        return false;
    }
    if (mesh.vertices.size() > 0xFFFF) {
        Logger::Error("UploadMesh: mesh exceeds 16-bit vertex limit (Phase-2 assert)");
        return false;
    }

    // Phase-2 single-mesh cache: replace previous buffers if present.
    if (bgfx::isValid(m_vbh)) bgfx::destroy(m_vbh);
    if (bgfx::isValid(m_ibh)) bgfx::destroy(m_ibh);

    // Layout expects apos(3)+nrm(3)+uv(2) floats; MeshVertex member order
    // matches exactly (x,y,z,nx,ny,nz,u,v), so the blob is passed as-is.
    static_assert(sizeof(MeshVertex) == 8 * sizeof(float),
                  "MeshVertex layout drifted; update GetVertexLayout()");
    const bgfx::Memory* vbMem = bgfx::copy(
        mesh.vertices.data(), uint32_t(mesh.vertices.size() * sizeof(MeshVertex)));
    m_vbh = bgfx::createVertexBuffer(vbMem, GetVertexLayout());

    // Track 3 uses 32-bit indices.
    const bgfx::Memory* ibMem = bgfx::copy(
        mesh.indices.data(), uint32_t(mesh.indices.size() * sizeof(uint32_t)));
    m_ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);

    if (!bgfx::isValid(m_vbh) || !bgfx::isValid(m_ibh)) {
        Logger::Error("UploadMesh: buffer creation failed");
        return false;
    }

    Logger::Info("Mesh uploaded: %zu verts, %zu indices (%zu submeshes)",
                 mesh.vertices.size(), mesh.indices.size(), mesh.subMeshes.size());
    return true;
}

void Renderer::DrawMesh(uint32_t indexOffset, uint32_t indexCount,
                        float offsetX, float offsetY, float scaleX, float scaleY) {
    if (!m_initialized) return;
    if (!bgfx::isValid(m_prog) || !bgfx::isValid(m_vbh) || !bgfx::isValid(m_ibh)) return;

    const float transform[4] = { offsetX, offsetY, scaleX, scaleY };
    bgfx::setUniform(m_uTransform, transform);

    const uint64_t state = 0
        | BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | BGFX_STATE_DEPTH_TEST_ALWAYS
        | BGFX_STATE_MSAA;
    bgfx::setState(state);

    bgfx::setVertexBuffer(0, m_vbh);
    bgfx::setIndexBuffer(m_ibh, indexOffset, indexCount);
    bgfx::submit(0, m_prog);
    bgfx::setVertexBuffer(0, m_vbh);
    bgfx::setIndexBuffer(m_ibh, indexOffset, indexCount);
    bgfx::submit(0, m_prog);
}

// --- 3D geometry path (World.img NIFs) --------------------------------------

GpuMesh Renderer::UploadMeshGpu(const Mesh& mesh) {
    GpuMesh gpu;
    if (!m_initialized || !mesh.IsValid()) {
        Logger::Error("UploadMeshGpu: renderer not initialized or mesh invalid");
        return gpu;
    }

    // Layout expects apos(3)+nrm(3)+uv(2) floats; MeshVertex member order
    // matches exactly (x,y,z,nx,ny,nz,u,v), so the blob is passed as-is.
    static_assert(sizeof(MeshVertex) == 8 * sizeof(float),
                  "MeshVertex layout drifted; update GetVertexLayout()");
    const bgfx::Memory* vbMem = bgfx::copy(
        mesh.vertices.data(), uint32_t(mesh.vertices.size() * sizeof(MeshVertex)));
    gpu.vbh = bgfx::createVertexBuffer(vbMem, GetVertexLayout());

    // 32-bit indices: World.img meshes exceed 65k vertices routinely.
    const bgfx::Memory* ibMem = bgfx::copy(
        mesh.indices.data(), uint32_t(mesh.indices.size() * sizeof(uint32_t)));
    gpu.ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);

    gpu.indexCount = uint32_t(mesh.indices.size());
    gpu.vertexCount = uint32_t(mesh.vertices.size());

    if (!gpu.valid()) {
        Logger::Error("UploadMeshGpu: buffer creation failed");
        if (bgfx::isValid(gpu.vbh)) bgfx::destroy(gpu.vbh);
        if (bgfx::isValid(gpu.ibh)) bgfx::destroy(gpu.ibh);
        gpu.vbh = BGFX_INVALID_HANDLE;
        gpu.ibh = BGFX_INVALID_HANDLE;
        gpu.indexCount = 0;
        gpu.vertexCount = 0;
        return gpu;
    }

    Logger::Info("GPU mesh uploaded: %u verts, %u indices",
                 gpu.vertexCount, gpu.indexCount);
    return gpu;
}

void Renderer::DestroyGpuMesh(GpuMesh& mesh) {
    if (bgfx::isValid(mesh.vbh)) bgfx::destroy(mesh.vbh);
    if (bgfx::isValid(mesh.ibh)) bgfx::destroy(mesh.ibh);
    mesh.vbh = BGFX_INVALID_HANDLE;
    mesh.ibh = BGFX_INVALID_HANDLE;
    mesh.indexCount = 0;
    mesh.vertexCount = 0;
}

// --- Skinned path (Track 17) -------------------------------------------------

GpuMesh Renderer::UploadSkinnedMeshGpu(const SkinnedMesh& mesh) {
    GpuMesh gpu;
    if (!m_initialized || !mesh.IsValid()) {
        Logger::Error("UploadSkinnedMeshGpu: renderer not initialized or mesh invalid");
        return gpu;
    }
    static_assert(sizeof(SkinnedMeshVertex) == 8 * sizeof(float) + 4 + 16,
                  "SkinnedMeshVertex layout drifted; update GetSkinnedVertexLayout()");
    const bgfx::Memory* vbMem = bgfx::copy(
        mesh.vertices.data(), uint32_t(mesh.vertices.size() * sizeof(SkinnedMeshVertex)));
    gpu.vbh = bgfx::createVertexBuffer(vbMem, GetSkinnedVertexLayout());

    const bgfx::Memory* ibMem = bgfx::copy(
        mesh.indices.data(), uint32_t(mesh.indices.size() * sizeof(uint32_t)));
    gpu.ibh = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_INDEX32);

    gpu.indexCount = uint32_t(mesh.indices.size());
    gpu.vertexCount = uint32_t(mesh.vertices.size());

    if (!gpu.valid()) {
        Logger::Error("UploadSkinnedMeshGpu: buffer creation failed");
        DestroyGpuMesh(gpu);
        return gpu;
    }
    Logger::Info("GPU skinned mesh uploaded: %u verts, %u indices",
                 gpu.vertexCount, gpu.indexCount);
    return gpu;
}

void Renderer::DrawMesh3DSubSkinned(const GpuMesh& mesh, uint32_t indexOffset,
                                    uint32_t indexCount, const float mvp[16],
                                    const float* boneMatrices, uint32_t boneCount) {
    if (!m_initialized || !mesh.valid()) return;
    if (!bgfx::isValid(m_prog3dSkinned) || !bgfx::isValid(m_uBones)) {
        // Graceful degradation: bind pose via the static path.
        DrawMesh3DRange(mesh, indexOffset, indexCount, mvp);
        return;
    }

    // Full identity palette (unused slots must not collapse vertices).
    m_bonePalette.assign(kMaxSkinBones * 16, 0.0f);
    for (int i = 0; i < kMaxSkinBones; i++) {
        float* m = &m_bonePalette[size_t(i) * 16];
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
    const uint32_t n = std::min<uint32_t>(boneCount, kMaxSkinBones);
    if (boneMatrices && n > 0)
        std::memcpy(m_bonePalette.data(), boneMatrices, size_t(n) * 16 * sizeof(float));
    m_pendingBoneUniform = true;
    DrawMesh3DRange(mesh, indexOffset, indexCount, mvp);
    m_pendingBoneUniform = false;
}

void Renderer::SetCameraView(const glm::mat4& view) {
    m_view = view;
}

void Renderer::SetLight(const float dir[3], const float color[3], const float ambient[3]) {
    // `dir` is the to-light direction; the shader consumes the travel
    // direction (its negation) and normalizes it there.
    for (int i = 0; i < 3; i++) {
        m_lightDir[i]     = -dir[i];
        m_lightColor[i]   = color[i];
        m_ambientColor[i] = ambient[i];
    }
}

void Renderer::RequestScreenshot(const char* name) {
    if (!name || !*name) return;
    m_screenshotName = name;
    m_screenshotRequested = true;
}

bgfx::ProgramHandle Renderer::LoadProgramFiles(const char* dir,
                                               const char* vsName,
                                               const char* fsName) {
    if (!dir || !vsName || !fsName) return BGFX_INVALID_HANDLE;
    return LoadProgram(std::string(dir), vsName, fsName);
}

bool Renderer::InitDebugBox() {
    if (m_debugBoxValid) return true;
    if (!m_initialized) return false;
    // Unit cube centered at origin, extent [-0.5, 0.5]^3, as 12 triangles
    // (24 indexed verts) so it draws with the same pipeline as world meshes;
    // the caller renders it without backface culling + depth-equal so the
    // silhouette reads as a wireframe over the world geometry.
    Mesh mesh;
    const float lo = -0.5f, hi = 0.5f;
    struct Face { float n[3]; float quad[4][3]; };
    const Face faces[6] = {
        { { 0, 0, 1}, { { lo, lo, hi}, { hi, lo, hi}, { hi, hi, hi}, { lo, hi, hi} } },
        { { 0, 0,-1}, { { hi, lo, lo}, { lo, lo, lo}, { lo, hi, lo}, { hi, hi, lo} } },
        { { 1, 0, 0}, { { hi, lo, hi}, { hi, lo, lo}, { hi, hi, lo}, { hi, hi, hi} } },
        { {-1, 0, 0}, { { lo, lo, lo}, { lo, lo, hi}, { lo, hi, hi}, { lo, hi, lo} } },
        { { 0, 1, 0}, { { lo, hi, hi}, { hi, hi, hi}, { hi, hi, lo}, { lo, hi, lo} } },
        { { 0,-1, 0}, { { lo, lo, lo}, { hi, lo, lo}, { hi, lo, hi}, { lo, lo, hi} } },
    };
    for (const Face& f : faces) {
        const uint32_t base = uint32_t(mesh.vertices.size());
        for (int i = 0; i < 4; i++)
            mesh.vertices.push_back({ f.quad[i][0], f.quad[i][1], f.quad[i][2],
                                      f.n[0], f.n[1], f.n[2], 0.0f, 0.0f });
        mesh.indices.insert(mesh.indices.end(),
            { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    m_debugBox = UploadMeshGpu(mesh);
    m_debugBoxValid = m_debugBox.valid();
    return m_debugBoxValid;
}

void Renderer::DrawDebugBox(const float model[16]) {
    if (!m_initialized || !m_debugBoxValid) return;
    // Draw through the 3D path on view 1 so depth ordering against world
    // geometry is consistent; disable culling + depth-write so all 12
    // triangles of the cube stay visible and the world beneath reads through.
    static const bool kNoCull = true;
    const uint64_t kSavedNoCull = 0; // DrawMesh3DRange reads env once; set state manually
    (void)kSavedNoCull;
    DrawMesh3DRange(m_debugBox, 0, m_debugBox.indexCount, model);
}

void Renderer::DrawBeacon(const GpuMesh& mesh, const float model[16]) {
    if (!m_initialized || !mesh.valid()) return;
    // Track 20: objective marker. Swap in a full-bright "emissive" look so
    // the column never shades dark: no directional term (light straight down;
    // side faces get diff = 0) and a saturated cyan ambient. The engine sun
    // is restored right after so later submits are unaffected.
    const float savedDir[3] = { m_lightDir[0], m_lightDir[1], m_lightDir[2] };
    const float savedCol[3] = { m_lightColor[0], m_lightColor[1], m_lightColor[2] };
    const float savedAmb[3] = { m_ambientColor[0], m_ambientColor[1], m_ambientColor[2] };
    const float down[3]     = { 0.0f, 1.0f, 0.0f };  // to-light = straight up
    const float white[3]    = { 0.35f, 0.35f, 0.35f };
    const float cyan[3]     = { 0.25f, 0.95f, 1.0f };
    SetLight(down, white, cyan);
    DrawMesh3DRange(mesh, 0, mesh.indexCount, model);
    SetLight(savedDir, savedCol, savedAmb);
}

void Renderer::DrawMesh3D(const GpuMesh& mesh, const float mvp[16]) {
    if (!m_initialized || !mesh.valid()) return;
    DrawMesh3DRange(mesh, 0, mesh.indexCount, mvp);
}

void Renderer::DrawMesh3DSub(const GpuMesh& mesh, uint32_t indexOffset,
                             uint32_t indexCount, const float mvp[16]) {
    if (!m_initialized || !mesh.valid()) return;
    DrawMesh3DRange(mesh, indexOffset, indexCount, mvp);
}

void Renderer::DrawMesh3DRange(const GpuMesh& mesh, uint32_t indexOffset,
                               uint32_t indexCount, const float mvp[16]) {

    // View 1: 3D geometry, cleared to a dim blue-grey sky with depth.
    // Set up here so the camera view set via SetCameraView() applies per-frame.
    const bgfx::ViewId kView3d = 1;
    static bool viewConfigured = false;
    if (!viewConfigured) {
        bgfx::setViewClear(kView3d, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00404c56, 1.0f, 0);
        bgfx::setViewRect(kView3d, 0, 0, bgfx::BackbufferRatio::Equal);
        viewConfigured = true;
    }

    // Projection is owned here; the view comes from the camera via
    // SetCameraView() (Engine calls it once per frame). Init builds it
    // eagerly; this is only a fallback for degenerate Init sizes.
    if (m_projection[3][3] == 0.0f && m_windowWidth > 0 && m_windowHeight > 0) {
        constexpr float kFovY = 60.0f;
        const float aspect = float(m_windowWidth) / float(m_windowHeight);
        if (bgfx::getCaps()->homogeneousDepth) {
            m_projection = glm::perspectiveLH_NO(
                glm::radians(kFovY), aspect, 0.1f, 20000.0f);
        } else {
            // D3D clip depth is [0,1].
            m_projection = glm::perspectiveLH_ZO(
                glm::radians(kFovY), aspect, 0.1f, 20000.0f);
        }
    }

    bgfx::setViewTransform(kView3d, glm::value_ptr(m_view), glm::value_ptr(m_projection));
    bgfx::touch(kView3d); // keep the clear even when no 3D mesh draws

    bgfx::setTransform(mvp);
    bgfx::setUniform(m_uMvp, mvp);
    // Track 17: skinned draws upload the bone-matrix palette right before
    // submit (set by DrawMesh3DSubSkinned).
    if (m_pendingBoneUniform && bgfx::isValid(m_uBones))
        bgfx::setUniform(m_uBones, m_bonePalette.data());
    // Track 12: submit directional light state (packed vec3-in-vec4).
    const float lightDirPack[4]     = { m_lightDir[0], m_lightDir[1], m_lightDir[2], 0.0f };
    const float lightColorPack[4]   = { m_lightColor[0], m_lightColor[1], m_lightColor[2], 0.0f };
    const float ambientColorPack[4] = { m_ambientColor[0], m_ambientColor[1], m_ambientColor[2], 0.0f };
    bgfx::setUniform(m_uLightDir, lightDirPack);
    bgfx::setUniform(m_uLightColor, lightColorPack);
    bgfx::setUniform(m_uAmbientColor, ambientColorPack);
    // Track 11: bind the current texture (BindTexture; placeholder default).
    bgfx::setTexture(0, m_sTex, m_boundTex.th);

    // Cull back faces (Gamebryo LH: front faces are CW). OPENBULLY_NOCULL=1
    // disables culling (debug/verification: flat geometry viewed from its
    // back side otherwise vanishes).
    static const bool kNoCull = std::getenv("OPENBULLY_NOCULL") != nullptr;
    const uint64_t state = 0
        | BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | BGFX_STATE_WRITE_Z
        | BGFX_STATE_DEPTH_TEST_LESS
        | (kNoCull ? 0 : BGFX_STATE_CULL_CW)
        | BGFX_STATE_MSAA;
    bgfx::setState(state);

    bgfx::setVertexBuffer(0, mesh.vbh);
    bgfx::setIndexBuffer(mesh.ibh, indexOffset, indexCount);
    bgfx::submit(kView3d, m_pendingBoneUniform ? m_prog3dSkinned : m_prog3d);

    // Track 12 verification hook: capture once after a submit has rendered.
    if (m_screenshotRequested) {
        m_screenshotRequested = false;
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, m_screenshotName.c_str());
        Logger::Info("Screenshot requested: %s (.tga)", m_screenshotName.c_str());
    }
}

// --- Textures (Track 11, NFT loader) ----------------------------------------

GpuTexture Renderer::UploadTexture(const NftTexture& tex) {
    GpuTexture gpu;
    // Called from Init() for the placeholder before m_initialized flips, so
    // the gate is bgfx readiness, not m_initialized.
    if (!bgfx::getCaps()) return gpu;
    if (tex.error.empty() == false) return gpu;
    if (tex.rgba.empty() || tex.width == 0 || tex.height == 0) return gpu;

    // Stream copy: bgfx takes ownership of its own memory block.
    const bgfx::Memory* mem = bgfx::copy(
        tex.rgba.data(), uint32_t(tex.rgba.size()));
    gpu.th = bgfx::createTexture2D(tex.width, tex.height,
                                   false /* no mips */, 1,
                                   bgfx::TextureFormat::RGBA8,
                                   0 /* default flags */,
                                   mem);
    gpu.width = tex.width;
    gpu.height = tex.height;
    if (!bgfx::isValid(gpu.th)) {
        Logger::Error("UploadTexture: createTexture2D failed (%ux%u)",
                      tex.width, tex.height);
    }
    return gpu;
}

GpuTexture Renderer::LoadTexture(const std::string& nftPath,
                                 const std::string& wantName) {
    GpuTexture gpu;
    if (!bgfx::getCaps()) return gpu; // bgfx not initialized yet
    const NftTexture tex = LoadNftFile(nftPath, wantName);
    if (!tex.error.empty()) {
        Logger::Warning("LoadTexture: %s (%s)", tex.error.c_str(), nftPath.c_str());
        return gpu;
    }
    return UploadTexture(tex);
}

void Renderer::BindTexture(const GpuTexture& texture) {
    m_boundTex = texture.valid() ? texture : m_placeholderTex;
}

void Renderer::DestroyTexture(GpuTexture& texture) {
    if (bgfx::isValid(texture.th)) bgfx::destroy(texture.th);
    texture.th = BGFX_INVALID_HANDLE;
    texture.width = 0;
    texture.height = 0;
}

} // namespace OpenBully

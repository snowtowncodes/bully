#include "renderer.h"
#include "../platform/logger.h"

#include <bgfx/bgfx.h>
#include <bx/bx.h>
#include <bx/math.h>
#include <fstream>
#include <string>
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

    // View 0: full backbuffer, cleared every frame. 0.1,0.1,0.15 -> 0x1a1a26.
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xff1a1a26, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, bgfx::BackbufferRatio::Equal);
    bgfx::setDebug(BGFX_DEBUG_TEXT);

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
    m_vbh = BGFX_INVALID_HANDLE;
    m_ibh = BGFX_INVALID_HANDLE;
    m_prog = BGFX_INVALID_HANDLE;
    m_uTransform = BGFX_INVALID_HANDLE;
    m_prog3d = BGFX_INVALID_HANDLE;
    m_uMvp = BGFX_INVALID_HANDLE;
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

void Renderer::SetCamera(const float eye[3], const float target[3], const float up[3]) {
    for (int i = 0; i < 3; i++) {
        m_eye[i] = eye[i];
        m_target[i] = target[i];
        m_up[i] = up[i];
    }
}

void Renderer::DrawMesh3D(const GpuMesh& mesh, const float mvp[16]) {
    if (!m_initialized || !mesh.valid()) return;

    // View 1: 3D geometry, cleared to a dim blue-grey sky with depth.
    // Set up here so camera state set via SetCamera() is applied per-frame.
    const bx::Vec3 eye    = bx::load<bx::Vec3>(m_eye);
    const bx::Vec3 target = bx::load<bx::Vec3>(m_target);
    const bx::Vec3 up     = bx::load<bx::Vec3>(m_up);

    const bgfx::ViewId kView3d = 1;
    static bool viewConfigured = false;
    if (!viewConfigured) {
        bgfx::setViewClear(kView3d, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x00404c56, 1.0f, 0);
        bgfx::setViewRect(kView3d, 0, 0, bgfx::BackbufferRatio::Equal);
        viewConfigured = true;
    }

    float view[16];
    bx::mtxLookAt(view, eye, target, up);

    float proj[16];
    constexpr float kFovY = bx::toDeg(60.0f);
    const float aspect = m_windowHeight > 0
        ? float(m_windowWidth) / float(m_windowHeight) : 1.0f;
    // NDC depth [0,1] for D3D; reversed not needed (standard depth test).
    bx::mtxProj(proj, kFovY, aspect, 0.1f, 20000.0f,
                bgfx::getCaps()->homogeneousDepth);

    bgfx::setViewTransform(kView3d, view, proj);
    bgfx::touch(kView3d); // keep the clear even when no 3D mesh draws

    bgfx::setTransform(mvp);
    bgfx::setUniform(m_uMvp, mvp);

    const uint64_t state = 0
        | BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | BGFX_STATE_WRITE_Z
        | BGFX_STATE_DEPTH_TEST_LESS
        | BGFX_STATE_CULL_CW  // Gamebryo LH coords: front faces are CW
        | BGFX_STATE_MSAA;
    bgfx::setState(state);

    bgfx::setVertexBuffer(0, mesh.vbh);
    bgfx::setIndexBuffer(mesh.ibh, 0, mesh.indexCount);
    bgfx::submit(kView3d, m_prog3d);
}

} // namespace OpenBully

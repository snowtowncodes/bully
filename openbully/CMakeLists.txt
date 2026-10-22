cmake_minimum_required(VERSION 3.20)
project(OpenBully VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Options
option(OPENBULLY_BUILD_TESTS "Build test suite" OFF)
option(OPENBULLY_BUILD_TOOLS "Build asset extraction tools" OFF)

# --- Dependencies -----------------------------------------------------------
# Windows: use the vendored SDL2 development package in external/ (see
# docs/building.md). Other platforms use the system SDL2.
set(OPENBULLY_SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/SDL2-2.30.12")

if(WIN32 AND EXISTS "${OPENBULLY_SDL2_DIR}/cmake/sdl2-config.cmake")
    find_package(SDL2 CONFIG REQUIRED
        PATHS "${OPENBULLY_SDL2_DIR}/cmake" NO_DEFAULT_PATH)
endif()

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 REQUIRED)
endif()

# --- bgfx (vendored bgfx.cmake release v1.129.8958-499 in external/bgfx) -----
# Source must be fetched first: scripts/fetch_bgfx.ps1 (see docs/dependencies.md)
set(OPENBULLY_BGPUX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/bgfx")
if(NOT EXISTS "${OPENBULLY_BGPUX_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "bgfx sources not found at external/bgfx.\n"
        "Run scripts/fetch_bgfx.ps1 to vendor bgfx.cmake v1.129.8958-499.")
endif()

# Trim bgfx.cmake build: we need the libraries + shaderc, nothing else.
set(BGFX_BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS           ON  CACHE BOOL "" FORCE)  # shaderc
set(BGFX_BUILD_TOOLS_SHADER    ON  CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_BIN2C     OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_GEOMETRY  OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_TEXTURE   OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL               OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS        OFF CACHE BOOL "" FORCE)
set(BGFX_LIBRARY_TYPE          STATIC CACHE STRING "" FORCE)
add_subdirectory("${OPENBULLY_BGPUX_DIR}" bgfx-build EXCLUDE_FROM_ALL)

# --- glm (vendored header-only 1.0.1, external/glm) --------------------------
set(OPENBULLY_GLM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/glm")
if(NOT EXISTS "${OPENBULLY_GLM_DIR}/glm/glm.hpp")
    message(FATAL_ERROR
        "glm sources not found at external/glm.\r\n"
        "Vendor glm 1.0.1 (headers only) so that external/glm/glm/glm.hpp exists.")
endif()
add_library(glm_headers INTERFACE)
target_include_directories(glm_headers INTERFACE "${OPENBULLY_GLM_DIR}")

# --- Main executable ---------------------------------------------------------
add_executable(openbully
    src/main.cpp
    src/engine/engine.cpp
    src/engine/resource_manager.cpp
    src/engine/formats/nif_loader.cpp
    src/engine/formats/nft_loader.cpp
    src/engine/formats/agr_loader.cpp
    src/animation/animation_controller.cpp
    src/entities/entity.cpp
    src/game/input.cpp
    src/game/camera.cpp
    src/physics/collision.cpp
    src/platform/window.cpp
    src/platform/logger.cpp
    src/rendering/mesh.cpp
    src/rendering/renderer.cpp
)

# NIF loader test harness (standalone, no SDL/bgfx dependency).
add_executable(nif_test
    src/tools/nif_test.cpp
    src/engine/formats/nif_loader.cpp
    src/rendering/mesh.cpp
    src/physics/collision.cpp
)
target_include_directories(nif_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENBULLY_GLM_DIR}
)

# Track 17 animation harness (.agr + skin + skeleton + pose; no GPU).
add_executable(anim_test
    src/tools/anim_test.cpp
    src/engine/formats/agr_loader.cpp
    src/engine/formats/nif_loader.cpp
    src/animation/animation_controller.cpp
    src/rendering/mesh.cpp
    src/physics/collision.cpp
)
target_include_directories(anim_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENBULLY_GLM_DIR}
)

# Collision self-test (Track 13; no GPU, no SDL/bgfx).
add_executable(collision_test
    src/tools/collision_test.cpp
    src/physics/collision.cpp
)
target_include_directories(collision_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(openbully PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(openbully PRIVATE
    SDL2::SDL2main
    SDL2::SDL2
    bgfx
    bx
    bimg
    glm_headers
)

# The vendored SDL2 VC package ships headers flat in include/ (SDL.h), while
# the codebase uses the portable <SDL2/SDL.h> spelling (matches vcpkg / Linux
# system layouts). Copy headers into <build>/sdl2-shim/SDL2 and add that dir.
if(WIN32 AND SDL2_DIR MATCHES "${CMAKE_CURRENT_SOURCE_DIR}/external")
    set(OPENBULLY_SDL2_SHIM_DIR "${CMAKE_BINARY_DIR}/sdl2-shim")
    file(COPY "${OPENBULLY_SDL2_DIR}/include/" DESTINATION "${OPENBULLY_SDL2_SHIM_DIR}/SDL2")
    target_include_directories(openbully PRIVATE "${OPENBULLY_SDL2_SHIM_DIR}")
endif()

# --- Shader compilation (shaderc from bgfx.cmake) ----------------------------
# bgfx consumes compiled shader binaries, not raw GLSL. Triangle shaders in
# src/rendering/shaders/*.sc are compiled per-backend into
# shaders/<backend>/*.bin next to the executable and loaded at runtime.
set(OPENBULLY_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/shaders")
set(OPENBULLY_SHADER_OUT_DIR "$<TARGET_FILE_DIR:openbully>/shaders")

set(OPENBULLY_SHADER_TARGETS "")
foreach(SHADER_NAME IN ITEMS vs_triangle fs_triangle vs_mesh3d fs_mesh3d vs_mesh3d_skinned)
    # Direct3D11 (SM 5.0) - pinned Phase-2 backend on Windows
    if(SHADER_NAME MATCHES "^vs_")
        set(SHADER_TYPE vertex)
    else()
        set(SHADER_TYPE fragment)
    endif()
    set(D3D11_OUT "${OPENBULLY_SHADER_OUT_DIR}/direct3d11/${SHADER_NAME}.bin")
    add_custom_command(
        OUTPUT  "${OPENBULLY_BINARY_DIR}/stamp_${SHADER_NAME}_d3d11"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENBULLY_SHADER_OUT_DIR}/direct3d11"
        COMMAND $<TARGET_FILE:shaderc>
                -f "${OPENBULLY_SHADER_DIR}/${SHADER_NAME}.sc"
                -o "${D3D11_OUT}"
                -i "${OPENBULLY_BGPUX_DIR}/bgfx/src" --varyingdef "${OPENBULLY_SHADER_DIR}/varying.def.sc"
                --platform windows
                --type ${SHADER_TYPE}
                --profile s_5_0
                -O 3
        DEPENDS shaderc
                "${OPENBULLY_SHADER_DIR}/${SHADER_NAME}.sc"
                "${OPENBULLY_SHADER_DIR}/varying.def.sc"
        COMMENT "shaderc: ${SHADER_NAME}.sc -> direct3d11"
        VERBATIM)
    list(APPEND OPENBULLY_SHADER_TARGETS "${OPENBULLY_BINARY_DIR}/stamp_${SHADER_NAME}_d3d11")
endforeach()
foreach(BACKEND IN ITEMS opengl vulkan)
    if(BACKEND STREQUAL opengl)
        set(PROFILE_VS "120")
        set(PROFILE_FS "120")
        set(PLATFORM "opengl")
    else()
        set(PROFILE_VS "spirv")
        set(PROFILE_FS "spirv")
        set(PLATFORM "vulkan")
    endif()
    foreach(SHADER_NAME IN ITEMS vs_triangle fs_triangle vs_mesh3d fs_mesh3d vs_mesh3d_skinned)
        if(SHADER_NAME MATCHES "^vs_")
            set(SHADER_TYPE vertex)
            set(SHADER_PROFILE ${PROFILE_VS})
        else()
            set(SHADER_TYPE fragment)
            set(SHADER_PROFILE ${PROFILE_FS})
        endif()
        set(OUT "${OPENBULLY_SHADER_OUT_DIR}/${BACKEND}/${SHADER_NAME}.bin")
        add_custom_command(
            OUTPUT  "${OPENBULLY_BINARY_DIR}/stamp_${SHADER_NAME}_${BACKEND}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENBULLY_SHADER_OUT_DIR}/${BACKEND}"
            COMMAND $<TARGET_FILE:shaderc>
                    -f "${OPENBULLY_SHADER_DIR}/${SHADER_NAME}.sc"
                    -o "${OUT}"
                    -i "${OPENBULLY_BGPUX_DIR}/bgfx/src" --varyingdef "${OPENBULLY_SHADER_DIR}/varying.def.sc"
                    --platform ${PLATFORM}
                    --type ${SHADER_TYPE}
                    --profile ${SHADER_PROFILE}
            DEPENDS shaderc
                    "${OPENBULLY_SHADER_DIR}/${SHADER_NAME}.sc"
                    "${OPENBULLY_SHADER_DIR}/varying.def.sc"
            COMMENT "shaderc: ${SHADER_NAME}.sc -> ${BACKEND}"
            VERBATIM)
        list(APPEND OPENBULLY_SHADER_TARGETS "${OPENBULLY_BINARY_DIR}/stamp_${SHADER_NAME}_${BACKEND}")
    endforeach()
endforeach()
if(WIN32 AND EXISTS "${OPENBULLY_SDL2_DIR}/lib/x64/SDL2.dll")
    add_custom_command(TARGET openbully POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENBULLY_SDL2_DIR}/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:openbully>/SDL2.dll"
        COMMENT "Copying SDL2.dll next to openbully.exe")
endif()

# Aggregate target for shader compilation: builds every per-shader stamp
# produced by the custom commands above. openbully depends on it so shader
# binaries always exist (and refresh) before the executable links/runs.
add_custom_target(openbully_shaders
    DEPENDS ${OPENBULLY_SHADER_TARGETS}
    COMMENT "Compiling bgfx shaders (shaderc)")
add_dependencies(openbully openbully_shaders)

# --- Subdirectories ----------------------------------------------------------
if(OPENBULLY_BUILD_TOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/CMakeLists.txt")
    add_subdirectory(src/tools)
endif()

if(OPENBULLY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()




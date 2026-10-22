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
    src/combat/combat.cpp
    src/game/boss.cpp
    src/vehicles/vehicle.cpp
    src/engine/resource_manager.cpp
    src/mission/mission.cpp
    src/photo/photo.cpp
    src/cutscene/cutscene.cpp   # Track 48: cutscene playback (camera keys + actor actions + subtitles)
    src/game/schedule.cpp
    src/game/classes.cpp   # Track 41: class system (attendance + completion)
    src/dialogue/dialogue.cpp
    src/engine/formats/nif_loader.cpp
    src/engine/formats/nft_loader.cpp
    src/engine/formats/agr_loader.cpp
    src/animation/animation_controller.cpp
    src/audio/audio.cpp
    src/entities/entity.cpp
    src/vehicles/vehicle.cpp
    src/vehicles/vehicle_physics.cpp
    src/vehicles/vehicle_feel.cpp
    src/game/tricks.cpp
    src/game/input.cpp
    src/game/camera.cpp
    src/minigame/minigame.cpp  # Track 44: minigame framework (class activities)
    src/game/boss.cpp          # Track 49: boss definitions + QTE helpers
    src/weather/weather.cpp
    src/physics/collision.cpp
    src/vehicles/vehicle_physics.cpp
    src/platform/window.cpp
    src/platform/logger.cpp
    src/platform/crash_report.cpp
    src/rendering/mesh.cpp
    src/rendering/renderer.cpp
    src/ui/ui.cpp
    src/engine/save_system.cpp
    src/arcade/arcade.cpp      # Track 45: arcade cabinets (Nut Shots / Monkey Fling)
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

# Track 32 vehicle physics probe (headless; no GPU/SDL).
add_executable(vp_probe
    src/tools/vp_probe.cpp
    src/vehicles/vehicle_physics.cpp
    src/physics/collision.cpp
)
target_include_directories(vp_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(vp_probe PRIVATE glm_headers)

# Track 46 trick scoring probe (headless; no GPU/SDL/glm).
add_executable(trick_probe
    src/tools/trick_probe.cpp
    src/game/tricks.cpp
)
target_include_directories(trick_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Track 33 vehicle handling self-test (headless; no GPU, no audio device).
add_executable(vehicle_test
    src/tools/vehicle_test.cpp
    src/vehicles/vehicle.cpp
    src/vehicles/vehicle_feel.cpp
    src/audio/audio.cpp
    src/platform/logger.cpp
)
target_include_directories(vehicle_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENBULLY_GLM_DIR}
)
if(WIN32 AND EXISTS "${OPENBULLY_SDL2_DIR}/cmake/sdl2-config.cmake")
    find_package(SDL2 CONFIG QUIET
        PATHS "${OPENBULLY_SDL2_DIR}/cmake" NO_DEFAULT_PATH)
endif()
if(TARGET SDL2::SDL2)
    target_link_libraries(vehicle_test PRIVATE SDL2::SDL2)
    target_compile_definitions(vehicle_test PRIVATE SDL_MAIN_HANDLED)
    if(WIN32 AND SDL2_DIR MATCHES "${CMAKE_CURRENT_SOURCE_DIR}/external")
        target_include_directories(vehicle_test PRIVATE "${CMAKE_BINARY_DIR}/sdl2-shim")
    endif()
endif()

# Track 21 audio self-test (no GPU; needs SDL audio + glm for the 3D API).
add_executable(audio_test
    src/tools/audio_test.cpp
    src/audio/audio.cpp
    src/platform/logger.cpp
)

# Track 41 class system self-test (headless; no GPU/SDL): JSON load, schedule
# queries, progression + save/load roundtrip of the class section.
add_executable(class_test
    src/tools/class_test.cpp
    src/game/classes.cpp
    src/engine/save_system.cpp
    src/platform/logger.cpp
)
target_include_directories(class_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENBULLY_GLM_DIR}
)
target_include_directories(audio_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENBULLY_GLM_DIR}
)
target_link_libraries(audio_test PRIVATE
    SDL2::SDL2
    # audio_test defines main() (not SDL_main); keep console subsystem.
    SDL2::SDL2main
)
if(WIN32 AND SDL2_DIR MATCHES "${CMAKE_CURRENT_SOURCE_DIR}/external")
    # OPENBULLY_SDL2_SHIM_DIR is defined further down (main target block);
    # this target is declared before it, so point at the shim path directly.
    target_include_directories(audio_test PRIVATE "${CMAKE_BINARY_DIR}/sdl2-shim")
endif()

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
foreach(SHADER_NAME IN ITEMS vs_triangle fs_triangle vs_mesh3d fs_mesh3d vs_mesh3d_skinned vs_particle fs_particle vs_ui fs_ui)
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
    foreach(SHADER_NAME IN ITEMS vs_triangle fs_triangle vs_mesh3d fs_mesh3d vs_mesh3d_skinned vs_particle fs_particle vs_ui fs_ui)
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

# Track 20: mission data next to the executable (runtime probes
# data/missions/demo_missions.json first, then the source tree).
add_custom_command(TARGET openbully POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:openbully>/data/missions"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/missions/demo_missions.json"
        "$<TARGET_FILE_DIR:openbully>/data/missions/demo_missions.json"
    COMMENT "Copying demo_missions.json next to openbully.exe")

# Track 29: NPC schedule data next to the executable (same probing
# convention as missions).
add_custom_command(TARGET openbully POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:openbully>/data/schedules"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/schedules/npc_schedules.json"
        "$<TARGET_FILE_DIR:openbully>/data/schedules/npc_schedules.json"
    COMMENT "Copying npc_schedules.json next to openbully.exe")

# Track 30: dialogue data next to the executable (same probe convention as
# missions: data/dialogue/demo_dialogue.json, then the source tree).
add_custom_command(TARGET openbully POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:openbully>/data/dialogue"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/dialogue/demo_dialogue.json"
        "$<TARGET_FILE_DIR:openbully>/data/dialogue/demo_dialogue.json"
    COMMENT "Copying demo_dialogue.json next to openbully.exe")

# Track 48: cutscene definitions next to the executable (same probe convention
# as missions: data/cutscenes/demo_cutscene.json, then the source tree).
add_custom_command(TARGET openbully POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:openbully>/data/cutscenes"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/cutscenes/demo_cutscene.json"
        "$<TARGET_FILE_DIR:openbully>/data/cutscenes/demo_cutscene.json"
    COMMENT "Copying demo_cutscene.json next to openbully.exe")

# Track 41: class definitions next to the executable (same probe convention:
# data/classes/classes.json first, then the source tree).
add_custom_command(TARGET openbully POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:openbully>/data/classes"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/data/classes/classes.json"
        "$<TARGET_FILE_DIR:openbully>/data/classes/classes.json"
    COMMENT "Copying classes.json next to openbully.exe")

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




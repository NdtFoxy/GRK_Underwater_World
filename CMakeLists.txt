cmake_minimum_required(VERSION 3.16)
project(project_underworld CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Place runtime in build/ root so it can find ./assets via working dir
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------
# Sources
# ---------------------------------------------------------------
set(APP_SOURCES
        src/main.cpp
        src/core/HeightmapLoader.cpp
        src/core/ModelLoader.cpp
        src/render/Framebuffer.cpp
        src/render/ShaderLoader.cpp
        src/scene/Scene.cpp
        src/scene/Scene_Terrain.cpp
        src/scene/Scene_Props.cpp
        src/scene/Scene_Render.cpp
        src/scene/VegetationSystem.cpp
)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_executable(${PROJECT_NAME} ${APP_SOURCES} ${IMGUI_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/external
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

# ---------------------------------------------------------------
# Dependencies — try CMake packages first, fall back to bundled
# (Windows: dependencies/ folder used by the original VS project)
# ---------------------------------------------------------------
find_package(OpenGL REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)

# GLEW
find_package(GLEW QUIET)
if (GLEW_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE GLEW::GLEW)
else()
    if (WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/dependencies/glew-2.0.0/include")
        target_include_directories(${PROJECT_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/dependencies/glew-2.0.0/include)
        target_link_directories(${PROJECT_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/dependencies/glew-2.0.0/lib/Release/Win32)
        target_link_libraries(${PROJECT_NAME} PRIVATE glew32)
    else()
        message(FATAL_ERROR "GLEW not found. Install via brew (macOS), apt (Linux) or vcpkg (Windows).")
    endif()
endif()

# GLFW
find_package(glfw3 QUIET)
if (glfw3_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE glfw)
else()
    if (WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/dependencies/glfw-3.3.8.bin.WIN32/include")
        target_include_directories(${PROJECT_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/dependencies/glfw-3.3.8.bin.WIN32/include)
        target_link_directories(${PROJECT_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/dependencies/glfw-3.3.8.bin.WIN32/lib-vc2019)
        target_link_libraries(${PROJECT_NAME} PRIVATE glfw3)
    else()
        message(FATAL_ERROR "GLFW not found. Install via brew (macOS), apt (Linux) or vcpkg (Windows).")
    endif()
endif()

# GLM (header-only)
find_package(glm QUIET)
if (glm_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE glm::glm)
else()
    if (EXISTS "${CMAKE_SOURCE_DIR}/dependencies/glm")
        target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/glm)
    else()
        message(FATAL_ERROR "GLM not found. Install via brew/apt/vcpkg or place headers in dependencies/glm.")
    endif()
endif()

# Assimp (model loading — OBJ / FBX)
find_package(assimp QUIET)
if (assimp_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE assimp::assimp)
else()
    if (WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/dependencies/assimp/include")
        target_include_directories(${PROJECT_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/dependencies/assimp/include)
        target_link_directories(${PROJECT_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/dependencies/assimp)
    else()
        message(FATAL_ERROR "Assimp not found. Install via brew (macOS), apt (Linux) or vcpkg (Windows).")
    endif()
endif()

# ---------------------------------------------------------------
# Platform tweaks
# ---------------------------------------------------------------
if (APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
            "-framework Cocoa"
            "-framework IOKit"
            "-framework CoreVideo")
endif()

if (WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# ---------------------------------------------------------------
# Copy assets next to the binary so relative paths work
# ---------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets)

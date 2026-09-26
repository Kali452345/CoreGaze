cmake_minimum_required(VERSION 3.20)
project(CoreGaze CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COREGAZE_VERSION_MAJOR 1)
set(COREGAZE_VERSION_MINOR 0)
set(COREGAZE_VERSION_PATCH 0)
set(COREGAZE_VERSION_BUILD 0)

option(ENABLE_CODE_SIGNING "Sign CoreGaze executable after build" OFF)
set(COREGAZE_SIGNTOOL "signtool.exe" CACHE STRING "Path to signtool executable")
set(COREGAZE_SIGN_CERT_PATH "" CACHE FILEPATH "Path to PFX certificate for signing")
set(COREGAZE_SIGN_CERT_PASSWORD "" CACHE STRING "Password for PFX certificate")
set(COREGAZE_SIGN_TIMESTAMP_URL "http://timestamp.digicert.com" CACHE STRING "RFC3161 timestamp server URL")

# Fetch Dear ImGui, pinned to a release commit so upstream changes can't break the build.
# To upgrade: pick a new release tag, replace the hash, rebuild, and check the HUD.
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        f1cc2ae15e53a861a874c3034aae6798fde194ab # v1.92.9b
)
FetchContent_MakeAvailable(imgui)

# Define source files including ImGui backends
set(SOURCES
    main.cpp
    SystemMonitor.cpp
    resource.rc
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    COREGAZE_VERSION_MAJOR=${COREGAZE_VERSION_MAJOR}
    COREGAZE_VERSION_MINOR=${COREGAZE_VERSION_MINOR}
    COREGAZE_VERSION_PATCH=${COREGAZE_VERSION_PATCH}
    COREGAZE_VERSION_BUILD=${COREGAZE_VERSION_BUILD}
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${imgui_SOURCE_DIR}>
    $<$<COMPILE_LANGUAGE:CXX>:${imgui_SOURCE_DIR}/backends>
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    dbghelp
    pdh
    iphlpapi
    ole32
    oleaut32
    wtsapi32
    ws2_32
)

if (WIN32 AND ENABLE_CODE_SIGNING)
    if (COREGAZE_SIGN_CERT_PATH STREQUAL "" OR COREGAZE_SIGN_CERT_PASSWORD STREQUAL "")
        message(WARNING "ENABLE_CODE_SIGNING is ON but COREGAZE_SIGN_CERT_PATH/COREGAZE_SIGN_CERT_PASSWORD is missing. Signing step will be skipped.")
    else()
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${COREGAZE_SIGNTOOL} sign /fd sha256 /td sha256 /tr "${COREGAZE_SIGN_TIMESTAMP_URL}" /f "${COREGAZE_SIGN_CERT_PATH}" /p "${COREGAZE_SIGN_CERT_PASSWORD}" "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Code signing CoreGaze executable"
            VERBATIM
        )
    endif()
endif()

# Add minimal build flags for performance
if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /O2 /MD)
elseif (MINGW OR CMAKE_COMPILER_IS_GNUCXX OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc -static-libstdc++)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -O2)
endif()
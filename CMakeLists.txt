cmake_minimum_required(VERSION 3.16)
project(wasmraw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR "$ENV{USERPROFILE}/imgui" CACHE PATH "Dear ImGui checkout (docking branch)")

# --- Vendored LibRaw decoding core (design doc 3.1: raw decoding, this is the
#     Phase 0 "start with LibRaw-Wasm" verification). DNG SDK, RawSpeed and
#     X3F sources are self-guarded out (#ifdef USE_DNGSDK / USE_RAWSPEED /
#     USE_X3FTOOLS) so we build them as no-ops for now.
file(GLOB_RECURSE LIBRAW_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/RawTherapee/rtengine/libraw/src/*.cpp"
)

add_library(rtlibraw STATIC ${LIBRAW_SOURCES})
target_include_directories(rtlibraw PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/RawTherapee/rtengine/libraw"
    "${CMAKE_CURRENT_SOURCE_DIR}/RawTherapee/rtengine/libraw/libraw"
)
target_compile_definitions(rtlibraw PRIVATE
    LIBRAW_NODLL
)

add_executable(wasmraw
    app/main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(wasmraw PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMGUI_DIR}/examples/libs/emscripten
    "${CMAKE_CURRENT_SOURCE_DIR}/app"
)

if(EMSCRIPTEN)
    set_target_properties(wasmraw PROPERTIES SUFFIX ".html")
    target_link_options(wasmraw PRIVATE
        "SHELL:-sUSE_GLFW=3"
        "SHELL:-sUSE_WEBGL2=1"
        "SHELL:-sFULL_ES3=1"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sNO_EXIT_RUNTIME=1"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','writeArrayToMemory','stringToUTF8']"
        "SHELL:-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_wasm_add_image','_wasm_select_image','_wasm_set_image_status','_wasm_control_hovered','_wasm_accept_decode','_wasm_accept_preview','_wasm_set_export_status']"
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/app/shell.html"
        "SHELL:-O2"
    )
else()
    find_package(glfw3 CONFIG REQUIRED)
    target_link_libraries(wasmraw PRIVATE glfw)
endif()

# --- Decoder worker module (LibRaw decode + full-res export, off the UI thread) ---
add_executable(decoder
    app/decoder.cpp
    app/third_party/stb_image_write.cpp
)

target_include_directories(decoder PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/app"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/third_party"
)

target_link_libraries(decoder PRIVATE rtlibraw)

if(EMSCRIPTEN)
    set_target_properties(decoder PROPERTIES SUFFIX ".js")
    target_link_options(decoder PRIVATE
        "SHELL:-sMODULARIZE=1"
        "SHELL:-sEXPORT_NAME=createDecoder"
        "SHELL:-sENVIRONMENT=worker"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sINITIAL_MEMORY=83886080"
        "SHELL:-sNO_EXIT_RUNTIME=1"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=['writeArrayToMemory','UTF8ToString','HEAPU8']"
        "SHELL:-sEXPORTED_FUNCTIONS=['_malloc','_free']"
        "SHELL:-O2"
    )
endif()

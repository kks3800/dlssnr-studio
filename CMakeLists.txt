cmake_minimum_required(VERSION 3.20)
project(dlssnr_image LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DirectML build of ONNX Runtime. The CPU build is still in the tree; RAFT was
# ~900 ms a frame on it, which was 96% of a render.
set(ORT ${CMAKE_CURRENT_SOURCE_DIR}/external/onnxruntime/onnxruntime-win-x64-dml-1.20.1)

add_library(dlssnr_core STATIC
    src/nr_runtime.cpp
    src/image_io.cpp
    src/depth_estimator.cpp
    src/sequence.cpp
    src/motion_estimator.cpp
    src/exr_io.cpp
    src/watermark.cpp
    src/video_stream.cpp
    external/tinyexr/miniz.c
)
target_include_directories(dlssnr_core PUBLIC src external/ngx external/tinyexr ${ORT}/include)
target_link_libraries(dlssnr_core PUBLIC d3d12 dxgi dxguid windowscodecs ole32
                                         ${ORT}/lib/onnxruntime.lib)

# The ORT runtime and the depth model have to sit beside whichever executable
# is being run, so stage them next to every target.
function(stage_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ORT}/lib/onnxruntime.dll $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ORT}/lib/DirectML.dll $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/external/models/depth-anything-v2-small.onnx
                $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/external/models/raft.onnx
                $<TARGET_FILE_DIR:${target}>)
endfunction()
target_compile_definitions(dlssnr_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if (EXISTS ${ORT}/include/dml_provider_factory.h)
    target_compile_definitions(dlssnr_core PUBLIC DLSSNR_HAVE_DIRECTML)
endif()

add_executable(dlssnr-image src/main.cpp)
target_link_libraries(dlssnr-image PRIVATE dlssnr_core)

# ---- GUI ----
set(IMGUI external/imgui)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${IMGUI}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI})
target_link_libraries(imgui PUBLIC d3d12 dxgi dxguid)

add_executable(dlssnr-studio WIN32 src/gui_main.cpp)
target_link_libraries(dlssnr-studio PRIVATE dlssnr_core imgui comdlg32)

stage_runtime(dlssnr-image)
stage_runtime(dlssnr-studio)

if (MSVC)
    target_compile_options(dlssnr_core   PRIVATE /W3 /permissive- /EHsc)
    target_compile_options(dlssnr-image  PRIVATE /W3 /permissive- /EHsc)
    target_compile_options(dlssnr-studio PRIVATE /W3 /permissive- /EHsc)
endif()

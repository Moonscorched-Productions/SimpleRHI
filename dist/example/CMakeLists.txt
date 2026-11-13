# SimpleRHI example with CMake.
#   cmake -B build -DSLANG_DIR=/path/to/slang-sdk [-DCMAKE_PREFIX_PATH=/path/to/SDL3]
#   cmake --build build
# The Slang shared library (slang.dll / libslang.so / libslang.dylib) must be findable when srhic runs:
# put it on PATH (Windows) or rely on the rpath set below (Linux, macOS).

cmake_minimum_required(VERSION 3.21)
project(srhi_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SIMPLERHI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Folder that contains SimpleRHI.h")
set(SLANG_DIR "" CACHE PATH "Slang SDK root (contains include/ and lib/)")
if(NOT SLANG_DIR)
    message(FATAL_ERROR "Set -DSLANG_DIR=/path/to/slang-sdk (needed to build the srhic shader compiler)")
endif()

find_package(SDL3 REQUIRED CONFIG)

# --- srhic: the shader compiler, built from one file ----------------------------------------------------
add_executable(srhic "${SIMPLERHI_DIR}/srhic.cpp")
target_include_directories(srhic PRIVATE "${SLANG_DIR}/include")
target_link_directories(srhic PRIVATE "${SLANG_DIR}/lib")
target_link_libraries(srhic PRIVATE slang)
set_target_properties(srhic PROPERTIES BUILD_RPATH "${SLANG_DIR}/lib")

# --- Shaders: one .srhs package per .slang file, copied next to the executable ----------------------------
function(srhi_compile_shader target source)
    get_filename_component(name "${source}" NAME_WE)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.srhs")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND srhic "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${output}" -I "${SIMPLERHI_DIR}"
        DEPENDS srhic "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${SIMPLERHI_DIR}/SimpleRHI.slang"
        COMMENT "srhic ${source}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${output}" "$<TARGET_FILE_DIR:${target}>/${name}.srhs")
endfunction()

# --- The application ----------------------------------------------------------------------------------------
add_executable(example main.cpp srhi_impl.cpp)
target_include_directories(example PRIVATE "${SIMPLERHI_DIR}")
target_link_libraries(example PRIVATE SDL3::SDL3)
srhi_compile_shader(example triangle.slang)

if(APPLE)
    # The Metal backend is Objective-C++ with ARC.
    set_source_files_properties(srhi_impl.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    target_link_libraries(example PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit")
elseif(WIN32 AND NOT MSVC)
    target_link_libraries(example PRIVATE d3d11 dxgi d3dcompiler dxguid) # MSVC links these automatically
endif()

if(WIN32)
    # Put SDL3.dll next to the executable when SDL3 is a shared library (a no-op for static builds).
    add_custom_command(TARGET example POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:example>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:example>;$<TARGET_FILE_DIR:example>>"
        COMMAND_EXPAND_LISTS)
endif()

# Builds the Skia bundled in a WPE WebKit source tree and skia-sampling, which
# draws an image scaled down with the sampling options WebKit uses (before
# and after webkit/0001-*.patch) on the CPU and on the GPU (EGL surfaceless).
#   cmake -S tests/imagescale -B /var/tmp/skia-test -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_CXX_COMPILER=clang++ -DWK=/path/to/wpewebkit-2.54.3
#   ninja -C /var/tmp/skia-test skia-sampling
cmake_minimum_required(VERSION 3.20)
project(skiasampling CXX C)
set(CMAKE_CXX_STANDARD 20)
if (NOT WK)
    message(FATAL_ERROR "-DWK=<wpewebkit source tree> is required")
endif ()
list(APPEND CMAKE_MODULE_PATH ${WK}/Source/cmake)
# Stand-ins for WebKit's compiler flag macros used by Skia's CMakeLists.txt
function(WEBKIT_ADD_TARGET_CXX_FLAGS target)
    foreach (flag ${ARGN})
        if (NOT flag MATCHES "dangling-reference|maybe-musttail|suggest-attribute|nontrivial-memcall|-Wno-attributes|psabi|redundant-move")
            target_compile_options(${target} PRIVATE ${flag})
        endif ()
    endforeach ()
endfunction()
function(WEBKIT_CHECK_COMPILER_FLAGS lang var)
    set(${var} ON PARENT_SCOPE)
endfunction()
function(WEBKIT_ADD_COMPILER_FLAGS lang kind src)
    set_property(SOURCE ${WK}/Source/ThirdParty/skia/${src} APPEND PROPERTY COMPILE_OPTIONS ${ARGN})
endfunction()
set(COMPILER_IS_CLANG ON)
set(WTF_CPU_X86_64 ON)
set(Skia_FRAMEWORK_HEADERS_DIR ${CMAKE_BINARY_DIR}/hdr)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(EGL REQUIRED)
add_subdirectory(${WK}/Source/ThirdParty/skia skia)
add_executable(skia-sampling skia-sampling.cpp ${WK}/Source/ThirdParty/skia/src/core/SkStrikeRef.cpp)
target_link_libraries(skia-sampling Skia ${EGL_LIBRARIES} GLESv2 freetype fontconfig png jpeg)

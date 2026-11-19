cmake_minimum_required(VERSION 3.27)
project(daylight LANGUAGES C CXX CUDA)   # C: tests/c_abi.c

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
# Own knob, not CMAKE_CUDA_ARCHITECTURES: project() (ours, or the server's before add_subdirectory) already cached nvcc's 52.
set(DAYLIGHT_CUDA_ARCHITECTURES "75-real;75-virtual" CACHE STRING "CUDA_ARCHITECTURES of src/gpu.cu (the OptiX-IR stays compute_75)")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(CUDAToolkit 12.6 REQUIRED)

# OptiX headers from the installed SDK (NVIDIA proprietary, never committed or fetched).
set(OPTIX_INSTALL_DIR "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.0.0" CACHE PATH "OptiX SDK root (contains include/optix.h)")
set(OPTIX_INCLUDE "${OPTIX_INSTALL_DIR}/include")
if(NOT EXISTS "${OPTIX_INCLUDE}/optix.h")
  message(FATAL_ERROR "optix.h not found under ${OPTIX_INCLUDE}; set OPTIX_INSTALL_DIR")
endif()

# earcut.hpp (ISC): n-gon triangulation.
include(FetchContent)
FetchContent_Declare(earcut GIT_REPOSITORY https://github.com/mapbox/earcut.hpp GIT_TAG v2.2.4 GIT_SHALLOW TRUE SOURCE_SUBDIR include)
FetchContent_MakeAvailable(earcut)

# OptiX device program -> OptiX-IR -> embedded byte array. Only place with --use_fast_math: the OptiX programs
# (visibility, transmittance, reflected-light paths, views' directions and plane distances; division and sqrt within
# ~2 ulp, denormals flushed).
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN}")
add_custom_command(OUTPUT "${GEN}/trace.optixir"
  COMMAND "${CMAKE_CUDA_COMPILER}" -optix-ir -arch=compute_75 --use_fast_math -m64 -std=c++17
          -ccbin "${CMAKE_CXX_COMPILER}" -I "${OPTIX_INCLUDE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/trace_dev.cu" -o "${GEN}/trace.optixir"
  DEPENDS src/trace_dev.cu src/params.h src/glass.h
  COMMENT "nvcc -optix-ir trace_dev.cu" VERBATIM)
add_custom_command(OUTPUT "${GEN}/trace_optixir.h"
  COMMAND "${CMAKE_COMMAND}" -DIN=${GEN}/trace.optixir -DOUT=${GEN}/trace_optixir.h -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS "${GEN}/trace.optixir" cmake/embed.cmake
  COMMENT "embed trace.optixir" VERBATIM)

set(CXX_WARN /W4 /WX /permissive- /fp:precise /utf-8 /EHsc)

add_library(daylightcore STATIC src/mesh.cpp src/sky.cpp src/solar.cpp src/run.cpp src/views.cpp src/cli.cpp src/gpu.cu "${GEN}/trace_optixir.h")
set_target_properties(daylightcore PROPERTIES CUDA_ARCHITECTURES "${DAYLIGHT_CUDA_ARCHITECTURES}")
target_include_directories(daylightcore PUBLIC src PRIVATE "${OPTIX_INCLUDE}" "${earcut_SOURCE_DIR}/include" "${GEN}")
target_link_libraries(daylightcore PUBLIC CUDA::cudart_static)
target_compile_definitions(daylightcore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(daylightcore PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:${CXX_WARN}>
  $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W3>)   # ponytail: /W3 on the one nvcc host TU, CUDA headers trip /W4 /WX

add_executable(daylight_cli src/main.cpp)
set_target_properties(daylight_cli PROPERTIES OUTPUT_NAME daylight)
target_link_libraries(daylight_cli PRIVATE daylightcore)
target_compile_definitions(daylight_cli PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(daylight_cli PRIVATE ${CXX_WARN})
target_link_options(daylight_cli PRIVATE /NODEFAULTLIB:libcmt)   # cudart_static pulls the static CRT; we are /MD

add_executable(views_cli src/main.cpp)   # the same main, the views flags and run
set_target_properties(views_cli PROPERTIES OUTPUT_NAME views)
target_link_libraries(views_cli PRIVATE daylightcore)
target_compile_definitions(views_cli PRIVATE VIEWS_CLI _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(views_cli PRIVATE ${CXX_WARN})
target_link_options(views_cli PRIVATE /NODEFAULTLIB:libcmt)

# The C ABI (src/daylight_c.h, docs/c-abi.md): daylight_c.dll beside the exes, what the C#, Python and Node bindings
# load; named so it never collides with a managed Daylight.dll. The static core inside, as in the exes (gpu.cu's OptiX
# function table once in this module, cudart_static, /MD).
add_library(daylight_c SHARED src/c_api.cpp)
target_link_libraries(daylight_c PRIVATE daylightcore)
target_include_directories(daylight_c INTERFACE src)
target_compile_definitions(daylight_c PRIVATE DAYLIGHT_C_BUILD _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(daylight_c PRIVATE ${CXX_WARN})
target_link_options(daylight_c PRIVATE /NODEFAULTLIB:libcmt)

file(GLOB CHECK_SRC CONFIGURE_DEPENDS tests/check*.cpp)
add_executable(check ${CHECK_SRC})
target_link_libraries(check PRIVATE daylightcore)
target_compile_definitions(check PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(check PRIVATE ${CXX_WARN})
target_link_options(check PRIVATE /NODEFAULTLIB:libcmt)

enable_testing()
add_test(NAME check COMMAND check "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_test(NAME cli_smoke COMMAND daylight_cli "${CMAKE_CURRENT_SOURCE_DIR}/tests/box.obj" --lat 41.98 --lon -87.92 --tz -6
         --quantity sunhours --time 06-21T12:00 --out "${CMAKE_CURRENT_BINARY_DIR}/smoke.txt")
add_test(NAME cli_smoke_npy COMMAND daylight_cli "${CMAKE_CURRENT_SOURCE_DIR}/tests/box.obj" --lat 41.98 --lon -87.92 --tz -6
         --quantity sunhours --time 06-21T12:00 --out "${CMAKE_CURRENT_BINARY_DIR}/smoke.npy")
add_test(NAME cli_smoke_clearsky COMMAND daylight_cli "${CMAKE_CURRENT_SOURCE_DIR}/tests/box.obj" --clear-sky --lat 41.98 --lon -87.92
         --quantity radiation --time 06-21T12:00 --out "${CMAKE_CURRENT_BINARY_DIR}/smoke_cs.txt")
add_test(NAME views_smoke COMMAND views_cli "${CMAKE_CURRENT_SOURCE_DIR}/tests/box.obj" --target ground=lawn --reach en17037
         --out "${CMAKE_CURRENT_BINARY_DIR}/views_smoke.txt")
add_test(NAME views_smoke_npy COMMAND views_cli "${CMAKE_CURRENT_SOURCE_DIR}/tests/box.obj" --fov 360 --offset 1.6 --sides both
         --dump-rays "${CMAKE_CURRENT_BINARY_DIR}/views_rays.npy" --out "${CMAKE_CURRENT_BINARY_DIR}/views_smoke.npy")
# The C ABI from plain C through the DLL (a static core here would be tested instead), against the two .npy smokes.
# Named c_abi.c, not check*.cpp: the glob above would pull it into check.
add_executable(c_abi tests/c_abi.c)
target_link_libraries(c_abi PRIVATE daylight_c)
target_compile_definitions(c_abi PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(c_abi PRIVATE /W4 /WX /utf-8)
add_test(NAME c_abi COMMAND c_abi "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(cli_smoke_npy views_smoke_npy PROPERTIES FIXTURES_SETUP cli_npy)
set_tests_properties(c_abi PROPERTIES FIXTURES_REQUIRED cli_npy TIMEOUT 300)

# examples/ (docs/cpp.md, docs/c-abi.md), built and run only when this is the top-level project: a host that adds this
# repo with add_subdirectory (the app server) gets the libraries without them.
if(PROJECT_IS_TOP_LEVEL)
  add_executable(example_cpp examples/cpp/example.cpp)
  target_link_libraries(example_cpp PRIVATE daylightcore)
  target_compile_options(example_cpp PRIVATE ${CXX_WARN})
  target_link_options(example_cpp PRIVATE /NODEFAULTLIB:libcmt)
  add_executable(example_c examples/c/example.c)
  target_link_libraries(example_c PRIVATE daylight_c)
  target_compile_definitions(example_c PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(example_c PRIVATE /W4 /WX /utf-8)
  add_test(NAME example_cpp COMMAND example_cpp "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME example_c COMMAND example_c "${CMAKE_CURRENT_SOURCE_DIR}/tests")
endif()

option(DAYLIGHT_USD "USD input for the CLI via bindings/cpp (fetches LightUSD)" OFF)
if(DAYLIGHT_USD)
  add_subdirectory(bindings/cpp)
  target_compile_definitions(daylight_cli PRIVATE DAYLIGHT_USD)
  target_link_libraries(daylight_cli PRIVATE daylight_usd)   # carries the bindings/cpp include dir
  target_compile_definitions(views_cli PRIVATE DAYLIGHT_USD)
  target_link_libraries(views_cli PRIVATE daylight_usd)
  add_test(NAME cli_usd COMMAND daylight_cli "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/box.usda" --lat 41.98 --lon -87.92 --tz -6
           --quantity sunhours --time 06-21T12:00 --out "${CMAKE_CURRENT_BINARY_DIR}/usd_smoke.txt")
endif()

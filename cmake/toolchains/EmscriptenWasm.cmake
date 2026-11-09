# RealScript + AeroGUI 2.0 Emscripten WebAssembly (Wasm32 + SIMD128 + WebGL2/WebGPU) Toolchain
# Usage:
#   cmake -B build-wasm -DCMAKE_TOOLCHAIN_FILE=third-party/RealScript/cmake/toolchains/EmscriptenWasm.cmake -DREALSCRIPT_SLIM_RUNTIME=ON

set(CMAKE_SYSTEM_NAME Emscripten)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

if(DEFINED ENV{EMSDK})
    set(EMSDK_ROOT "$ENV{EMSDK}/upstream/emscripten")
    if(WIN32)
        set(CMAKE_C_COMPILER "${EMSDK_ROOT}/emcc.bat")
        set(CMAKE_CXX_COMPILER "${EMSDK_ROOT}/em++.bat")
        set(CMAKE_AR "${EMSDK_ROOT}/emar.bat")
        set(CMAKE_RANLIB "${EMSDK_ROOT}/emranlib.bat")
    else()
        set(CMAKE_C_COMPILER "${EMSDK_ROOT}/emcc")
        set(CMAKE_CXX_COMPILER "${EMSDK_ROOT}/em++")
        set(CMAKE_AR "${EMSDK_ROOT}/emar")
        set(CMAKE_RANLIB "${EMSDK_ROOT}/emranlib")
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce zero-exception, zero-ASYNCIFY, Wasm SIMD128 + WebGL2/WebGPU flags:
# - Strictly forbid -sASYNCIFY=1 (prevents 50%+ Wasm code size bloat and callstack overhead)
# - Enable -fno-exceptions and -msimd128 for 1,000+ ECS HUD Track B SIMD updates
set(AERO_WASM_COMPILE_FLAGS "-fno-exceptions -msimd128 -DNDEBUG")
set(AERO_WASM_LINK_FLAGS "-sWASM=1 -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1 -sDISABLE_EXCEPTION_CATCHING=1 -sASYNCIFY=0 -sFILESYSTEM=0 -sENVIRONMENT=web,worker")

set(CMAKE_C_FLAGS_INIT "${AERO_WASM_COMPILE_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${AERO_WASM_COMPILE_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${AERO_WASM_LINK_FLAGS}")

set(REALSCRIPT_SLIM_RUNTIME ON CACHE BOOL "Use slim AOT/VM runtime for Wasm targets" FORCE)
set(REALSCRIPT_BUILD_TESTS OFF CACHE BOOL "Disable desktop test runner under Wasm toolchain" FORCE)
set(REALSCRIPT_INSTALL_SDK OFF CACHE BOOL "Disable SDK install under Wasm toolchain" FORCE)

# SDK Productization

RealScript RS0 defines the first versioned embedding surface for engine
integration. v0.2.0 ships that surface as a release, while the language and
binary compatibility contracts remain unfrozen.

## CMake consumer targets

The build exports component targets so hosts can declare intent without depending on the internal library layout:

```cmake
find_package(RealScript CONFIG REQUIRED)

target_link_libraries(game PRIVATE
    RealScript::Runtime
    RealScript::GameRuntime
)
```

Available targets are:

- `RealScript::Runtime`
- `RealScript::Compiler`
- `RealScript::GameRuntime`
- `RealScript::GameCompiler`
- `RealScript::GameSdk`
- `RealScript::AotSupport`
- `RealScript::Debug`
- `RealScript::Tooling`
- `RealScript::Full`
- `RealScript::Frontend` for compatibility
- `RealScript::rsaot` as the installed AOT generator executable

The v0.2.0 implementation still uses one internal static archive. The component
targets are the supported consumer boundary; the implementation can be split
later without changing downstream CMake files.

Install and consume the SDK:

```bash
cmake -S . -B build -DREALSCRIPT_INSTALL_SDK=ON
cmake --build build
cmake --install build --prefix /path/to/realscript-sdk
```

The package installs headers, command-line tools, `RealScriptConfig.cmake`, exported targets, version metadata, the license, and `RealScriptAot.cmake`. The exported `RealScript::rsaot` executable target allows an installed consumer to include `${RealScript_AOT_MODULE}` and call `realscript_add_aot_library` without referring back to the source tree.

## SDK version contract

`realscript/Version.h` exposes separate version dimensions for:

- the project version;
- embedding compatibility;
- the Game SDK bytecode-package identity contract;
- the restricted script-object state format.

Hosts should pin RealScript to an exact release or commit while the ABI remains
unfrozen.

## Loading verified bytecode modules

`GameProgramLoader` accepts one or more encoded `.rsbc` modules, performs defensive decoding and verification, links a `ProgramImage`, and returns a `GameProgramPackage` containing:

- the linked program;
- host bindings;
- managed heap and native-handle registry;
- a canonical program-content hash;
- a canonical host-API hash.

```cpp
realscript::game::GameApi api = buildHostApi();
realscript::game::GameProgramLoader loader(api);
auto loaded = loader.loadBytecodeModules(encodedModules);
if (!loaded.succeeded()) {
    // Report loaded.diagnostics.
}

auto runtime = loaded.package.createRuntime();
```

`stableProgramContentHash()` sorts modules canonically and hashes their verified bytecode plus SDK compatibility versions. `stableGameApiHash()` hashes compiler-visible generated host declarations. Game engines can combine both values into content identity, save compatibility, multiplayer handshakes, and cache keys.

## Restricted deterministic object state

Managed heap slots, `ObjectRef`, and `NativeHandle` values are process-local and must not be stored directly in saves or rollback checkpoints. RS0 adds a restricted state contract:

```cpp
auto state = snapshotScriptObject(runtime, object, error);
auto bytes = encodeScriptObjectState(*state, error);
auto decoded = decodeScriptObjectState(bytes, error);
restoreScriptObject(runtime, restoredObject, *decoded, error);
```

The default policy accepts:

- void/default values;
- null strings;
- Boolean, integer, long, enum, and string values;
- structs whose nested fields are also accepted.

The default policy rejects:

- managed object and array references;
- native handles;
- floating-point values unless explicitly enabled;
- excessive field counts, nesting, or encoded size.

State encoding is little-endian, versioned, bounded, duplicate-field checked, and protected by a canonical state hash. Restore requires an exact script type and field layout match.

## Compatibility boundary

RS0 does not freeze the source language, `.rsbc` format, object ABI, or native module ABI. It establishes the APIs an engine can build on while continuing to pin the exact RealScript SDK identity. A future compatibility bump must update the constants in `Version.h` and the related regression fixtures.

## License status

RealScript is distributed under the Apache License 2.0. The SDK installation includes the repository `LICENSE` file under the platform data directory.

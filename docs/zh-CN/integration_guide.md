# RealScript 游戏脚本引擎完整接入指南（v0.98 + P0–P3 全栈版）

本文档面向 C++17 游戏与仿真引擎研发团队，系统整理 RealScript 在合入 **P0（紧凑拆箱堆与可信 VM 快路径）**、**P1（DAP 纯函数求值沙箱与 LSP 场景清单桥接）**、**P2（原位结构热重载、确定性多核 `ParallelJob` 与 AOT SIMD、纤程自动下沉与无锁帧遥测、内存态 `StencilJit`）** 以及 **P3（七层物理构建与 ABI 契约治理、格式化器/静态 Linter/差分模糊测试、模式匹配与接口/注解/扩展方法演进、`RealScript.Reflect` 反射与 `ModHost` 插件沙箱）** 后的完整接入方式与工程最佳实践。

## 1. 架构分层与 CMake 构建接入

P3 阶段 1 将原先的单体实现拆分为 7 个严格无环的物理构建层（`foundation`、`compiler_core`、`runtime_core`、`native_backends`、`debug`、`tooling`、`game`），同时完整保留 `RealScript::*` 消费端目标契约。你可以根据宿主进程形态按需链接对应组件：

| CMake 导入目标 | 适用进程形态 | 包含的核心能力范围 |
| :--- | :--- | :--- |
| `RealScript::Runtime` | 纯运行态轻量宿主 / 独立服务器 | `ManagedHeap`、字节码解释器、`ParallelJobScheduler`、`FiberScheduler`、`AsyncRuntime`、`FrameTelemetryCollector`、`AbiGovernance` |
| `RealScript::Compiler` | 离线烘焙器 / 热编译服务 | 词法/语法/语义前端（含模式匹配、默认接口、强类型注解、扩展方法）、MIR 优化器、`ModuleBuildCache`、`.rsbc` 编解码与校验 |
| `RealScript::AotSupport` | 发布态原生 C++17 AOT 客户端 | `AotRuntime` 零预算可信运行库、`__restrict` 紧凑数组 SIMD 算子、C11 `NativeModuleAbi` |
| `RealScript::GameSdk` | 引擎主工程（场景 + ECS + 绑定） | `GameApi`、`SceneScriptRuntime`、零拷贝 ECS 原生列、`ResourceRegistry`、`GameProductization`、`ReflectionRegistry`、`ModHost` |
| `RealScript::Debug` | 编辑器调试宿主 / 远程附加进程 | `DebugSession`、`EmbeddedDapServer`（TCP / stdio DAP、无副作用求值、虚拟 Fiber 线程栈、流式帧遥测） |
| `RealScript::Tooling` | IDE 语言服务 / CI 代码治理工具 | `LanguageService`、`LspServer`、场景/信号/资产 GUID 清单桥接、`Formatter`、`GovernanceLinter`、`GovernanceFuzzer` |
| `RealScript::Full` | 一体化编辑器 + Play-In-Editor | 上述全部运行态、编译态、JIT、调试与工具链能力 |

### 1.1 源码子目录接入与 SDK 安装包接入

在引擎根 `CMakeLists.txt` 中，可通过源码子目录或预安装 SDK 两种方式引入：

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyGameEngine LANGUAGES C CXX)

# 方式 A：作为源码子目录内嵌构建
set(REALSCRIPT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REALSCRIPT_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory(external/RealScript)

# 方式 B：使用 cmake --install 导出的预编译 SDK
# find_package(RealScript 0.98 CONFIG REQUIRED)

add_executable(MyEngineEditor src/editor_main.cpp)
target_link_libraries(MyEngineEditor PRIVATE RealScript::Full)
target_compile_features(MyEngineEditor PRIVATE cxx_std_17)
```

### 1.2 发布态 C++17 AOT 静态库自动生成

对于正式发包客户端，推荐使用 `cmake/RealScriptAot.cmake` 提供的 `realscript_add_aot_library`，在构建期直接将 `.rscript` 编译为开启零预算可信快路径与 `__restrict` SIMD 的原生 C++17 静态库：

```cmake
include(external/RealScript/cmake/RealScriptAot.cmake)

realscript_add_aot_library(
    mygame_scripts_aot
    PROGRAM_NAME MyGameScripts
    CPP_NAMESPACE mygame::scripts_aot
    OPT_LEVEL 2
    TRUSTED_RELEASE
    CACHE_DIR "${CMAKE_BINARY_DIR}/rs_module_cache"
    SOURCES
        scripts/core_math.rscript
        scripts/combat_systems.rscript
        scripts/ai_director.rscript
)

add_executable(MyGameShippingClient src/shipping_main.cpp)
target_link_libraries(MyGameShippingClient PRIVATE
    mygame_scripts_aot
    RealScript::GameRuntime
    RealScript::AotSupport
)
```

## 2. ABI、字节码与快照契约门禁（`AbiGovernance`）

在加载离线烘焙的 `.rsbc` 字节码包、跨动态库 AOT 插件或网络回滚存档前，应在引擎启动与资产加载入口执行二进制契约校验，防止因结构尺寸或字节码版本失配引发未定义行为。

```cpp
#include "realscript/AbiGovernance.h"
#include "realscript/game/GameProductization.h"

bool validateAndLoadScriptPackage(
    const std::vector<std::uint8_t>& rawRsbcBytes,
    const std::string& packageContractJson) {
    // 1. 16 字节 .rsbc 文件头零分配快速预检（支持 0.6..0.9 版本协商与损坏拦截）
    const auto headerCheck = realscript::inspectBytecodeHeaderCompatibility(rawRsbcBytes);
    if (!headerCheck.canLoad()) {
        // headerCheck.issues 包含截断、非法魔数或不支持的目标版本诊断
        return false;
    }

    // 2. 校验打包端与当前宿主引擎的二进制 ABI 清单（Value 16B、Instruction 8B、RSGS 版本、FNV-1a 摘要）
    std::string parseErr;
    const auto artifactManifest = realscript::AbiContractManifest::fromJson(packageContractJson, &parseErr);
    if (!artifactManifest) {
        return false;
    }

    const auto hostManifest = realscript::currentAbiContractManifest();
    const auto compat = realscript::verifyAbiCompatibility(hostManifest, *artifactManifest);
    if (!compat.canLoad()) {
        // compat.status == AbiCompatibilityStatus::Incompatible
        // 可输出 compat.summary() 到引擎日志面板
        return false;
    }

    return true;
}
```

如果使用 `GameProgramLoader` 加载产品化脚本包，可直接调用带 ABI 门禁报告的重载版本，加载器会自动填充 `GameProgramLoadResult` 中的兼容性诊断信息。

## 3. 宿主 C++ 绑定、场景生命周期与零拷贝 ECS

### 3.1 用 `rsbind` 从 C++ 头文件自动生成绑定与 `.d.rscript` 声明桩

RealScript 内置零外部依赖的单文件 C++ 声明解析器（`RsBindParser.h`，支持 `#define`/`#if` 常量求值、命名空间、继承、重载消歧、`std::vector<T>` 与自定义值类型 `struct` 封送）。在工程构建脚本或自定义 CMake 命令中运行 `rsbind`：

```bash
rsbind \
  -DENGINE_PLATFORM_PC=1 \
  --header engine/include/GameplayHostTypes.h \
  --output-cpp engine/generated/GameplayHostBindings.gen.cpp \
  --output-stub scripts/ambient/EngineHost.d.rscript
```

也可以在 C++ 中通过 `GameApi` 直接声明宿主类型、方法、属性与引用计数资源桥接：

```cpp
#include "realscript/game/GameScripting.h"
#include "realscript/game/ResourceHandle.h"

void registerEngineApis(realscript::game::GameApi& api) {
    // 注册全局确定性宿主函数
    api.function("Engine.Math", "ClampInt", +[](int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    });

    // 导出标准 .d.rscript 供离线编译器 rsc 与编辑器 LSP 智能补全共用
    realscript::compiler::SourceFile hostStub = api.exportDeclarationStub("EngineHost.d.rscript");
    (void)hostStub;
}
```

### 3.2 场景脚本生命周期与零拷贝原生 ECS 列绑定

`SceneScriptRuntime` 采用扁平化 `FlatLifecycleSlot` 缓存表驱动 `fixedUpdate`、`update` 与 `lateUpdate`，并支持将 C++ 连续结构体数组零拷贝映射为脚本侧 `RawColumnArray`，消除每帧编组拷贝：

```cpp
#include "realscript/game/GameScripting.h"

struct NativeTransformColumn {
    double posX;
    double posY;
    double posZ;
};

void stepEngineScene(
    realscript::game::SceneScriptRuntime& scene,
    std::vector<NativeTransformColumn>& transforms,
    double fixedDt,
    double frameDt) {
    // 1. 原位零拷贝绑定宿主连续内存列（每帧重绑原位复用紧凑槽位，零堆分配）
    scene.scriptRuntime().bindEcsRawColumnAuto<NativeTransformColumn>(
        "TransformColumn", transforms.data(), transforms.size());

    // 2. 驱动场景实体组件生命周期
    scene.fixedUpdate(fixedDt);
    scene.update(frameDt);
    scene.lateUpdate(frameDt);

    // 3. 利用帧末剩余微秒预算推进分代/增量 GC（严控单帧停顿）
    std::vector<realscript::runtime::Value> extraRoots;
    scene.scriptRuntime().heap().collectIncrementalWithBudgetMicroseconds(extraRoots, 250);
}
```

## 4. 多档位执行后端：解释器快路径、`StencilJit` 与确定性多核 `ParallelJob`

RealScript 在所有执行档位共享统一的 16 字节 `Value`（首偏移载荷、末字节标签）与 P0 紧凑拆箱数组（`int[]` 4B/元素、`double[]` 8B/元素、`bool[]` 1B/元素），可按运行平台自由组合执行策略：

### 4.1 零外部依赖内存态 `StencilJit`（开发态 / 热更态 / 无外部编译器客户端）

`StencilJit` 采用 `PROT_READ | PROT_WRITE` 写入后切换 `PROT_READ | PROT_EXEC` 的严格 `W^X` 封页机制，在约 50 微秒内将字节码直接编译为原生 `x86_64` 机器码（非 `x86_64` 架构自动回退至直接线索化模板，不支持原生化的宿主调用/异常函数按函数粒度无缝回退至解释器）：

```cpp
#include "realscript/jit/Jit.h"

void runWithInMemoryStencilJit(
    std::shared_ptr<const realscript::runtime::ProgramImage> program,
    std::shared_ptr<realscript::runtime::ManagedHeap> heap,
    std::shared_ptr<const realscript::runtime::BindingRegistry> bindings) {
    realscript::jit::StencilJit jit;
    realscript::jit::StencilJitOptions jitOpts;
    jitOpts.enableNativeCodegen = true;   // 启用 x86_64 W^X 原生机器码生成
    jitOpts.enableThreadedFallback = true;

    auto compiled = jit.compile(program, jitOpts);
    if (!compiled.succeeded()) {
        return;
    }

    compiled.module->setHeap(heap);
    compiled.module->setBindings(bindings);

    realscript::runtime::ExecutionOptions execOpts;
    execOpts.limits.instructionBudget = 10'000'000;
    auto result = compiled.module->invoke("Game.Sim::StepSimulation", {realscript::runtime::Value{std::int64_t{1024}}}, execOpts);
    (void)result;
}
```

### 4.2 确定性多核无锁数据并行 `ParallelJobScheduler`

针对寻路代价场、群体避障（Boids）、伤害结算与粒子模拟等数据密集型逻辑，`ParallelJobScheduler` 在分发前静态验证整个调用图无堆分配、无静态写、无动态多态与无跨参数别名，随后进入 `ManagedHeap::ParallelLockFreeScope` 零锁并发执行，并按分片编号 `0 .. numBatches - 1` 固定顺序归约，确保 `1`、`4`、`8` 或任意线程数在 `DeterminismMode::Strict` 下结果与摘要逐位一致：

```csharp
// 脚本侧核函数（simulation_jobs.rscript）
module Game.Jobs;

public extern int ParallelRandomRange(int minInclusive, int maxInclusive);

// 元素核：第 0 个参数为 int index，后续参数为紧凑数组或只读标量
public int IntegrateVelocity(int index, double[] posX, double[] velX, double dt) {
    int jitter = ParallelRandomRange(-1, 1);
    posX[index] = posX[index] + velX[index] * dt + jitter * 0.001;
    return 1;
}
```

```cpp
#include "realscript/runtime/ParallelJob.h"

void dispatchParallelVelocityJob(
    std::shared_ptr<const realscript::runtime::ProgramImage> program,
    std::shared_ptr<realscript::runtime::ManagedHeap> heap,
    realscript::runtime::ObjectRef posXArray,
    realscript::runtime::ObjectRef velXArray) {
    auto bindings = std::make_shared<realscript::runtime::BindingRegistry>();
    // 注册按分片独立播种（baseSeed ^ chunkIndex）的确定性并行随机数绑定
    realscript::runtime::registerParallelPrngBindings(*bindings, "Game.Jobs");

    // 可在加载阶段预先执行一次静态字节码无堆校验
    auto verifyReport = realscript::runtime::verifyParallelJobFunction(
        *program, "Game.Jobs::IntegrateVelocity", {.bindings = bindings.get()});
    if (!verifyReport.valid) {
        // verifyReport.summaryMessage() 指出违规指令与函数位置
        return;
    }

    realscript::runtime::ParallelJobScheduler scheduler(/*defaultWorkerCount=*/0); // 0 自动按硬件核心数开启线程池
    realscript::runtime::ParallelJobConfig cfg;
    cfg.batchSize = 128;
    cfg.baseSeed = 0x20261112u;
    cfg.reduceOp = realscript::runtime::ParallelReduceOp::Sum;
    cfg.executionOptions.determinism = realscript::runtime::DeterminismMode::Strict;

    const std::size_t entityCount = 4096;
    auto jobRes = scheduler.dispatchFor(
        program,
        heap,
        "Game.Jobs::IntegrateVelocity",
        entityCount,
        {realscript::runtime::Value{posXArray}, realscript::runtime::Value{velXArray}, realscript::runtime::Value{0.016}},
        cfg,
        bindings);
    // jobRes.value == 4096, jobRes.determinismDigest 在任意线程数下恒定一致
}
```

## 5. 原位结构热重载与双轨协程调度

### 5.1 原位 `CompactSlot` 结构热重载与 `[FormerlySerializedAs]`

在开发期修改脚本类结构（增删重排字段、重命名字段、拓宽数值类型、增删虚函数或接口实现）时，开启 `ReloadOptions::allowStructuralChanges = true` 调用 `hot_reload::apply` 即可在安全点原位重映射堆槽位，保持所有外部持有的 `ObjectRef`（`slot`、`generation`、`heapId`）恒等不变：

```csharp
// 修改后的脚本：重命名字段并拓宽类型，同时新增带默认值的字段
module Game.Actors;

public class HeroActor {
    [FormerlySerializedAs("hp")]
    public double Health = 100.0; // 自动从旧版 int hp 受检转换保留原数值

    public int Shield = 25;       // 新增字段，存活实例自动填充常量默认值 25
}
```

```cpp
#include "realscript/hot_reload/HotReload.h"

bool applyLiveStructuralReload(
    realscript::runtime::EngineRuntime& engine,
    std::vector<realscript::bytecode::Module> newlyCompiledModules) {
    realscript::hot_reload::ReloadOptions opts;
    opts.allowStructuralChanges = true;
    opts.allowModuleSetChanges = true;
    opts.allowAddedFunctions = true;

    auto plan = realscript::hot_reload::apply(engine, std::move(newlyCompiledModules), opts);
    if (!plan.compatible) {
        // 若超出堆内存上限，已自动无损原子回滚，不会污染任何存活对象
        return false;
    }
    // plan.migrationStats 记录了迁移对象数、重命名字段数、委托虚槽重绑数与写屏障补记数
    return true;
}
```

### 5.2 双轨协程与纯值 `sequence` 自动下沉 `FiberScheduler`

RealScript 严格区分两条异步轨道以避免帧同步回滚污染：
- **确定性回滚轨（`sequence` 与 `RealScript.Fiber`）**：用于技能时间轴、AI 行为树与关卡剧情。对纯标量/定点数状态机，编译期可通过 `FiberCompiler::canAutoLowerToFiber` 与 `compileAutoLoweredModule` 直接降级为 8 字节紧凑指令流，交由零指针 POD 布局的 `FiberScheduler` 批量步进，支持微秒级二进制快照与帧回滚。
- **主线程异步 I/O 轨（`RealScript.Async` `async`/`await`）**：用于资产流式加载、存档读写与 HTTP/WebSocket 请求，通过 `AsyncRuntimeContext` 在主线程 `PreTickIo`、`FixedTick`、`Update`、`EndOfFrame` 四个确定阶段集中恢复，实体销毁时自动级联取消 `CancellationToken`。

## 6. 调试器、无锁帧遥测、LSP 场景桥接与 CI 代码治理

### 6.1 内嵌 DAP 调试服务器与零分配环形帧遥测（`FrameTelemetryCollector`）

在引擎进程内启动 `EmbeddedDapServer` 并绑定 `FiberScheduler` 与 `FrameTelemetryCollector`，VS Code 插件（`tools/vscode-realscript`）附加后不仅支持无副作用纯 Getter Watch/Hover 求值与条件断点，还能直接查看成百上千个挂起 Fiber 的虚拟线程调用栈与源码行号，并实时接收每帧耗时热点推送：

```cpp
#include "realscript/debug/DapServer.h"
#include "realscript/runtime/Runtime.h"

class EngineDebugTelemetryBridge {
public:
    void initialize(
        std::shared_ptr<const realscript::runtime::ProgramImage> program,
        std::shared_ptr<realscript::runtime::ManagedHeap> heap,
        std::shared_ptr<realscript::debug::DebugSession> debugSession,
        const realscript::runtime::FiberScheduler* fibers,
        const realscript::runtime::CompiledFiberModule* fiberMod) {
        dapServer_ = std::make_unique<realscript::debug::EmbeddedDapServer>(
            program, heap, debugSession);
        dapServer_->server().bindFiberScheduler(fibers, fiberMod);
        dapServer_->server().bindFrameTelemetry(&telemetry_);
        dapServer_->start(/*port=*/4711); // VS Code 可直接通过 TCP 4711 端口 Attach
    }

    void runFrame(
        std::uint64_t frameIndex,
        realscript::runtime::EngineRuntime& engine,
        const realscript::runtime::FiberScheduler* fibers) {
        telemetry_.beginFrame(frameIndex);
        {
            realscript::runtime::ActiveFrameTelemetryScope scope(&telemetry_);
            (void)engine.invoke("Game.Loop::OnFrameTick");
        }
        // 零堆分配写入 128 帧 Seqlock 环形槽位，聚合 Top-8 热点函数、紧凑堆/装箱堆字节与 GC 切片耗时
        auto sample = telemetry_.endFrame(&engine.heap(), nullptr, fibers);
        (void)sample;
    }

private:
    realscript::runtime::FrameTelemetryCollector telemetry_;
    std::unique_ptr<realscript::debug::EmbeddedDapServer> dapServer_;
};
```

在 VS Code 调试控制台输入 `:telemetry` 可即时拉取最近帧剖析报表，调试流也会输出 `realscript/frameTelemetry` 实时事件。

### 6.2 `realscript.project.json` 场景感知 LSP 与静态治理 Linter 配置

在脚本工程根目录放置 `realscript.project.json`，`rslsp` 语言服务器与 `rsc` 编译器即可自动加载宿主 `.d.rscript` 声明桩、场景节点/信号/资产 GUID 智能补全清单，以及 `RSG001`–`RSG005` 静态工程治理规则：

```json
{
  "ambientPaths": [
    "scripts/ambient/EngineHost.d.rscript",
    "scripts/ambient/RealScript.Reflect.d.rscript"
  ],
  "sceneFiles": [
    "assets/scenes/main_arena.rsscene"
  ],
  "enableGovernanceLint": true,
  "lint": {
    "maxCyclomaticComplexity": 18,
    "rules": {
      "RSG001": "error",
      "RSG002": "warning",
      "RSG003": "warning",
      "RSG004": "error",
      "RSG005": "warning"
    }
  }
}
```

5 条内置静态治理规则的检查语义如下：

| 规则代码 | 规则标识 | 拦截隐患说明 | 行内按需豁免写法 |
| :--- | :--- | :--- | :--- |
| `RSG001` | `hot-path-allocation` | 在 `OnUpdate`/`OnFixedUpdate`/`OnLateUpdate`/`Update`/`FixedUpdate` 中执行引用类型 `new` 或堆闭包捕获 | `// realscript-disable-next-line RSG001` |
| `RSG002` | `cyclomatic-complexity` | 单个函数/方法的分支、循环、`case`、`catch` 与短路逻辑圈复杂度超出设定阈值 | `// realscript-disable-next-line RSG002` |
| `RSG003` | `unused-symbol` | 未使用的局部变量（`_` 前缀除外）、未读取的 `private` 字段及冗余 `import` 模块 | `// realscript-disable-next-line RSG003` |
| `RSG004` | `sequence-float-equality` | 在确定性回滚 `sequence` 协程内对 `float`/`double` 直接使用 `==` 或 `!=` 精确比较 | `// realscript-disable-next-line RSG004` |
| `RSG005` | `suspicious-control-flow` | 无任何注释说明的空 `catch` 吞异常块，或条件两侧代码完全相同的 `if`/`else` 分支 | `// realscript-disable-next-line RSG005` |

在 CI 流水线中可直接调用 `rsc` 新增的治理参数设置提交门禁，并在回归测试中调用 `GovernanceFuzzer` 执行多后端差分与字节码损坏模糊测试：

```bash
# 1. 检查代码格式是否符合规范（不一致返回非零退出码；本地自动排版用 rsc --format --write）
rsc --check-format scripts/combat_systems.rscript

# 2. 执行静态治理规则检查并将告警提升为阻断错误
rsc --project realscript.project.json --lint --lint-deny-warnings scripts/combat_systems.rscript
```

## 7. P3 现代语言扩展特性速查

所有 P3 前端语言扩展均在编译期降级为标准 Typed MIR，在**字节码解释器、`StencilJit` 与 C++17 AOT** 三端具备一致行为：

```csharp
module Game.ExtensionsDemo;

// 1. 自定义强类型注解契约：[AttributeUsage] 校验挂载目标与重复次数
[AttributeUsage("class|method", allowMultiple: false)]
public class CombatSkillAttribute {
    public string Category;
    public CombatSkillAttribute(string category) { Category = category; }
}

// 2. 默认接口实现（Trait Mixin）：无需深层单继承即可为 class 与 struct 混入公共行为
public interface IDamageReceiver {
    int Armor();

    int CalcFinalDamage(int raw) {
        int net = raw - Armor();
        return net > 0 ? net : 0;
    }
}

public struct ShieldUnit : IDamageReceiver {
    public int Defense;
    public int Armor() { return Defense; }
}

// 3. 泛型扩展方法与 `ref this` 可变值类型接收者扩展方法
public void BoostDefense(ref this ShieldUnit unit, int bonus) {
    unit.Defense = unit.Defense + bonus; // 直接原地修改调用方左值结构体字段
}

public T EchoIdentity<T>(this T value) {
    return value;
}

[CombatSkill("burst")]
public class DamageEvaluator {
    // [Obsolete] 支持温和演进告警（RS4101）或硬性废弃报错（第二个参数传 true 报 RS4102）
    [Obsolete("请改用 EvaluateUnit", false)]
    public static int LegacyEval(int hp) { return hp; }

    // 4. 关系模式、逻辑组合子（and/or/not）与嵌套结构化属性模式
    public static int EvaluateUnit(ShieldUnit? maybeUnit, int incomingHit) {
        if (maybeUnit is null) {
            return -1;
        }
        if (maybeUnit is >= 0) {
            // Nullable 自动解包支持
        }
        ShieldUnit u = maybeUnit.Value();
        u.BoostDefense(5);

        int finalHit = u.CalcFinalDamage(incomingHit);
        return finalHit switch {
            <= 0 => 0,
            > 0 and < 50 => 1,
            >= 50 and < 100 => 2,
            _ => 3
        };
    }
}
```

## 8. 确定性元数据反射 `RealScript.Reflect` 与沙箱化 `ModHost`

### 8.1 `ReflectionRegistry` 与脚本侧能力门控 `RealScript.Reflect`

当需要动态探测脚本类结构或允许受信任工具脚本访问反射元数据时，在 C++ 端构建 `ReflectionRegistry` 并按需开放 `"reflection"` 能力：

```cpp
#include "realscript/reflection/Reflection.h"

void setupReflectionBridge(
    std::shared_ptr<const realscript::runtime::ProgramImage> program,
    std::shared_ptr<realscript::runtime::ManagedHeap> heap,
    realscript::runtime::BindingRegistry& bindings) {
    auto refl = realscript::reflection::ReflectionRegistry::fromProgram(program, heap);

    // 注册脚本侧 RealScript.Reflect 绑定，要求调用上下文显式具备 "reflection" 能力
    realscript::reflection::registerReflectBindings(bindings, refl, "reflection", heap);

    // 宿主侧可直接查询全程序带特定注解的字段/方法/类型
    for (const auto& fieldMatch : refl->findFieldsWithAttribute("Export")) {
        // fieldMatch.ownerType->canonicalName, fieldMatch.field->name
        (void)fieldMatch;
    }
}
```

编译脚本时把 `realscript::reflection::exportReflectDeclarationStub()` 加入输入文件列表，脚本即可 `import RealScript.Reflect;`，使用 `Reflect.TypeNameOf(obj)`、`Reflect.HasAttribute(target, attr)`、`Reflect.GetAttributeString(target, attr, key)`、`Reflect.GetFieldCount(type)`、`Reflect.GetFieldName(type, idx)` 以及 `Reflect.GetIntField` / `SetIntField`、`GetStringField` / `SetStringField`、`GetBoolField` / `SetBoolField`、`GetDoubleField` / `SetDoubleField`。未在 `ExecutionOptions::capabilities` 中授予 `"reflection"` 的不可信脚本调用上述函数时，会被运行时直接拦截并返回 `ErrorCode::CapabilityDenied`。

### 8.2 多 Mod SemVer 拓扑沙箱宿主 `ModHost`

`ModHost` 专为创意工坊 / UGC 脚本插件设计，提供四重隔离保障：
1. **SemVer 字典序拓扑依赖求解**：通过 `parseModManifestJson` 与 `resolveModLoadPlan` 解析 `^1.2.0`、`~1.2.3`、`>=1.0.0 <2.0.0` 等版本约束，确定性输出唯一加载序列，并精确诊断缺包、版本冲突及循环依赖环路径。
2. **编译期 AST 安全守卫**：通过 `addCompilerExtensionPass`（如内置 `makeForbiddenImportPass`、`makeForbiddenSyntaxPass`）在编译第三方 Mod 源码时拦截敏感模块导入或受限语法。
3. **独立堆与指令配额熔断器**：每个 Mod 拥有受 `maxHeapBytes` 硬上限约束的私有 `ManagedHeap`、独立 `EngineRuntime` 与单次调用 `tickInstructionBudget`。一旦某 Mod 死循环或内存爆表，连续达到 `faultThreshold` 即转入 `ModState::Tripped` 自动短路隔离，宿主与其他健康 Mod 不受影响。
4. **零悬垂跨堆深拷贝事件与 RPC 总线**：`publishEvent` 与 `invokeModEndpoint` 底层通过 `marshalCrossHeapValue` 在隔离堆之间深拷贝标量、内联数学值（`Fixed`/`Fixed16`/`Vec2`/`Vec3`/`Quat`）、字符串、纯值 `struct` DTO 及值数组，同时**严格拒绝一切跨堆 `ObjectRef`（`class` 引用）与 `NativeHandle`**（返回 `MarshalErrorCode::CrossHeapReferenceRejected`），从根源杜绝某个 Mod 卸载或 GC 后在外部留下悬垂引用。

每个 Mod 包包含一份 `mod.json` 与对应 `.rscript` 脚本：

```json
{
  "id": "workshop.loot_plus",
  "version": "1.1.0",
  "dependencies": {
    "core.items": "^1.0.0"
  },
  "grantedCapabilities": [],
  "maxHeapBytes": 2097152,
  "tickInstructionBudget": 40000,
  "entryModule": "LootPlus.Entry"
}
```

```csharp
module LootPlus.Entry;

public struct DropContextDto {
    public int MonsterLevel;
    public int PlayerLuck;
}

// 使用 [EventHandler] / [Subscribe] / [OnEvent] 自动订阅跨堆事件总线
[EventHandler("world.monster_slain")]
public void OnMonsterSlain(DropContextDto ctx) {
    // 独立堆与 40,000 指令预算保护下运行
}

// 使用 [ModEndpoint] / [Export] / [Rpc] 自动暴露跨 Mod 服务端点
[ModEndpoint("ComputeDropCount")]
public int ComputeDropCount(int monsterLevel, int luck) {
    return monsterLevel >= 10 && luck >= 5 ? 3 : 1;
}
```

宿主引擎加载并驱动多个 Mod 的标准流程如下：

```cpp
#include "realscript/plugin/ModHost.h"

void integrateWorkshopMods(const std::vector<realscript::plugin::ModPackage>& packages) {
    realscript::plugin::ModHostConfig config;
    config.faultThreshold = 2;
    config.optimizationLevel = realscript::optimization::Level::Aggressive;
    config.mountReflectModuleByDefault = true;

    realscript::plugin::ModHost modHost(config);

    // 挂载只读宿主声明模块与编译期禁止导入守卫
    modHost.addCompilerExtensionPass(
        realscript::plugin::makeForbiddenImportPass({"Engine.AdminFilesystem"}));

    const auto loadReport = modHost.loadMods(packages);
    if (!loadReport.succeeded) {
        return;
    }

    // 广播游戏事件到所有活跃订阅者（已熔断 Mod 会自动跳过并计入 skippedTrippedCount）
    auto pub = modHost.publishEvent(
        "world.monster_slain",
        {},
        &modHost.hostHeap());
    (void)pub;

    // 点对点调用指定 Mod 暴露的端点并将返回值深拷贝回宿主堆
    auto ep = modHost.invokeModEndpoint(
        "workshop.loot_plus",
        "ComputeDropCount",
        {realscript::runtime::Value{std::int64_t{12}}, realscript::runtime::Value{std::int64_t{8}}},
        &modHost.hostHeap(),
        &modHost.hostHeap());
    if (ep.succeeded) {
        std::int64_t drops = std::get<std::int64_t>(ep.value);
        (void)drops;
    }
}
```

## 9. 引擎主循环全链路集成参考骨架

下面给出将 **ABI 校验、核心场景运行时、内存态 `StencilJit`、帧遥测、内嵌 DAP 调试器、结构热重载与 `ModHost` 创意工坊沙箱** 串联进同一引擎生命周期的标准骨架：

```cpp
#include "realscript/AbiGovernance.h"
#include "realscript/debug/DapServer.h"
#include "realscript/game/GameScripting.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/jit/Jit.h"
#include "realscript/plugin/ModHost.h"
#include "realscript/reflection/Reflection.h"
#include "realscript/runtime/ParallelJob.h"

class RealScriptEngineSubsystem {
public:
    bool startup(
        const std::vector<realscript::compiler::SourceFile>& gameSources,
        const std::vector<realscript::plugin::ModPackage>& workshopMods,
        bool enableLiveDebugger = true) {
        // 1. 编译核心游戏脚本并链接 ProgramImage
        realscript::compiler::Compilation compilation(gameSources);
        auto build = compilation.build();
        if (build.diagnostics.hasErrors()) return false;

        realscript::optimization::Options optOpts{realscript::optimization::Level::Aggressive};
        auto opt = realscript::optimization::Optimizer{}.optimize(
            std::move(build.modules), build.diagnostics, optOpts);
        if (build.diagnostics.hasErrors()) return false;

        realscript::bytecode::Lowerer bcLowerer;
        std::vector<realscript::bytecode::Module> bcModules;
        for (const auto& m : opt.modules) bcModules.push_back(bcLowerer.lower(m));

        realscript::runtime::RuntimeError linkErr;
        auto linked = realscript::runtime::ProgramImage::link(std::move(bcModules), linkErr);
        if (!linked) return false;

        auto program = std::make_shared<const realscript::runtime::ProgramImage>(std::move(*linked));
        heap_ = std::make_shared<realscript::runtime::ManagedHeap>();
        bindings_ = std::make_shared<realscript::runtime::BindingRegistry>();
        realscript::runtime::registerParallelPrngBindings(*bindings_);

        // 2. 构建反射表并挂载能力门控的 RealScript.Reflect
        reflection_ = realscript::reflection::ReflectionRegistry::fromProgram(program, heap_);
        realscript::reflection::registerReflectBindings(*bindings_, reflection_, "reflection", heap_);

        runtime_ = std::make_unique<realscript::runtime::EngineRuntime>(program, heap_, bindings_);

        // 3. 编译内存态 StencilJit 加速热循环（约 50us 完成）
        realscript::jit::StencilJit sjit;
        auto jitRes = sjit.compile(program);
        if (jitRes.succeeded()) {
            stencilModule_ = jitRes.module;
            stencilModule_->setHeap(heap_);
            stencilModule_->setBindings(bindings_);
        }

        // 4. 启动内嵌 TCP DAP 调试器并绑定无锁帧遥测
        if (enableLiveDebugger) {
            debugSession_ = std::make_shared<realscript::debug::DebugSession>(program, heap_);
            dap_ = std::make_unique<realscript::debug::EmbeddedDapServer>(program, heap_, debugSession_);
            dap_->server().bindFrameTelemetry(&telemetry_);
            dap_->start(4711);
        }

        // 5. 加载并隔离第三方创意工坊 Mod
        modHost_ = std::make_unique<realscript::plugin::ModHost>();
        if (!workshopMods.empty()) {
            (void)modHost_->loadMods(workshopMods);
        }
        return true;
    }

    void tickFrame(std::uint64_t frameNo, double dt) {
        telemetry_.beginFrame(frameNo);
        {
            realscript::runtime::ActiveFrameTelemetryScope telScope(&telemetry_);
            if (stencilModule_) {
                (void)stencilModule_->invoke("Game.Main::Tick", {realscript::runtime::Value{dt}});
            } else {
                (void)runtime_->invoke("Game.Main::Tick", {realscript::runtime::Value{dt}});
            }
        }
        if (modHost_) {
            (void)modHost_->publishEvent("engine.post_tick", {realscript::runtime::Value{dt}}, &modHost_->hostHeap());
        }
        // 帧末微秒级增量 GC 切片与遥测封帧
        std::vector<realscript::runtime::Value> extraRoots;
        const auto gcStart = std::chrono::steady_clock::now();
        heap_->collectIncrementalWithBudgetMicroseconds(extraRoots, 200);
        const auto gcNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - gcStart).count());
        telemetry_.recordGcSliceNanoseconds(gcNs);
        (void)telemetry_.endFrame(heap_.get());
    }

    void shutdown() {
        if (modHost_) modHost_->unloadAll();
        if (dap_) dap_->stop();
    }

private:
    std::shared_ptr<realscript::runtime::ManagedHeap> heap_;
    std::shared_ptr<realscript::runtime::BindingRegistry> bindings_;
    std::shared_ptr<realscript::reflection::ReflectionRegistry> reflection_;
    std::unique_ptr<realscript::runtime::EngineRuntime> runtime_;
    std::shared_ptr<realscript::jit::StencilModule> stencilModule_;
    realscript::runtime::FrameTelemetryCollector telemetry_;
    std::shared_ptr<realscript::debug::DebugSession> debugSession_;
    std::unique_ptr<realscript::debug::EmbeddedDapServer> dap_;
    std::unique_ptr<realscript::plugin::ModHost> modHost_;
};
```

# 游戏脚本 SDK

Game Scripting SDK 将 RealScript 编译器、运行时与 C++17 游戏对象、ECS 或场景系统连接起来，提供：

- C++ 自由函数、类成员方法和属性的类型化绑定，以及 `Fixed`/`Fixed16`/`Vec2`/`Vec3` 数学原语直接封送与零分配快路径（`getMemberVec2` / `setMemberVec2`）；
- 编译器可见的宿主 API 声明与 `.d.rs` 声明桩导出（`exportDeclarationStub`）；
- 使用 `NativeHandle` 的安全原生对象包装、脚本类直接继承 C++ `NativeType`、多态解包与显式生命周期释放（`releaseNative` / `rebindNative`）；
- C++ 创建、持有和调用脚本对象的 Host API，以及 Interpreter / JIT / AOT 分层执行策略（`TieredExecutionPolicy`）；
- 单实体多组件挂载（`attachComponent` / `getComponent`）、Editor 与 Play 双执行模式（`[Tool]` / `[ExecuteInEditMode]`）；
- `OnCreate`、`OnStart`、`OnEnable`、`OnDisable`、`OnValidate`、`OnUpdate`、`OnFixedUpdate`、`OnLateUpdate`、`OnDestroy` 完整场景生命周期；
- 深度 `InspectorTypeSchema` 反射（覆盖嵌套 Struct、Enum、泛型容器、`[EntityRef]`/`[ComponentRef]`/`[AssetRef]` 与 `event` 信号签名）、`setInspectorMember` 自动触发 `OnValidate`、宿主跨实体信号连接（`connectSignal`）；
- 带状态结构化热重载（`SceneScriptRuntime::hotReload`，支持增删字段、`[FormerlySerializedAs]` 字段重命名、协程/事件状态保留及跨组件引用自动修补）；
- 定向或广播事件、事件队列、条件触发器、执行预算、确定性策略以及结构化错误。

完整接口和示例见英文主文档 [GAME_SCRIPTING_SDK.md](../en/GAME_SCRIPTING_SDK.md)。

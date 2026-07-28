# 游戏脚本 SDK

Game Scripting SDK 将 RealScript 编译器、运行时与 C++17 游戏对象、ECS 或场景系统连接起来，提供：

- C++ 自由函数、类成员方法和属性的类型化绑定；
- 编译器可见的宿主 API 声明；
- 使用 `NativeHandle` 的安全原生对象包装；
- C++ 创建、持有和调用脚本对象的 Host API；
- `OnCreate`、`OnStart`、`OnUpdate`、`OnDestroy` 等场景生命周期；
- 定向或广播事件、事件队列和条件触发器；
- 执行预算、确定性策略以及结构化错误。

完整接口和示例见英文主文档 [GAME_SCRIPTING_SDK.md](../en/GAME_SCRIPTING_SDK.md)。

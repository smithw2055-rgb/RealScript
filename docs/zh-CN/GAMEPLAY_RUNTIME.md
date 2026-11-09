# 确定性游戏运行时

本阶段在现有 Game Scripting SDK 之上补齐面向 RTS、塔防、回放和 Rollback 的游戏运行时基础。

## Phase 8：确定性基础

新增 `GameplayPrimitives`：

- 带代际校验的 `EntityId`；
- 固定 Tick 时钟和每帧追赶预算；
- 独立 PCG 随机流；
- 按 Tick 与稳定 ID 排序的计时器；
- 类型化确定性事件队列；
- 状态快照与稳定 Hash。

这些结构不使用裸指针、系统时间、平台随机数或无序容器遍历来决定模拟顺序。

## Phase 9：脚本游戏层

新增：

- `GameplayHost`：实体、计时器、事件（含 `subscribeOnce` 一次性事件订阅与 `cancelEntityCalls` 实体级定时器清理）、随机流和脚本调用的统一宿主；
- `SceneGameplayDriver`：驱动 `SceneScriptRuntime` 的固定帧入口；
- 命名事件订阅与发布；
- 确定性脚本协程 `sequence`：支持 `yield wait_ticks(n)`、`yield wait_seconds(s)`、`yield wait_event(topic)` 与 `yield wait_until(cond)`，并在组件禁用或卸载时自动取消挂起协程；
- `ScriptContract`，用于按回调名称和参数数量验证行为、技能、任务和 AI 脚本；
- `ScriptMetadataRegistry` 与 `InspectorTypeSchema`：自动从编译器源码 Attribute 导入，支持嵌套 Struct、Enum、容器、引用分类（`[EntityRef]`/`[ComponentRef]`/`[AssetRef]`）与 `event` 信号反射；
- `installGameplayBindings`，向脚本生成 `CurrentTick`、`TickRate`、实体、随机数、计时器（`Schedule`、`CancelTimer`、`CancelEntityTimers`）和事件（`SubscribeOnce`、`Publish`）API。

每个 Tick 的固定顺序为：

1. 推进 Tick；
2. 处理到期计时器和确定性事件；
3. 调用脚本回调；
4. 执行 `OnFixedUpdate`；
5. 评估触发器；
6. 刷新场景事件队列。

## Phase 10：存档、回放与 Rollback

新增版本化 `RSGS` 二进制状态格式，覆盖：

- Entity slot 与 generation；
- Tick、累计时间与丢帧统计；
- 随机流状态；
- 计时器、重复次数和脚本调用参数；
- 待处理事件与订阅关系；
- 脚本序列所有权；
- 稳定状态 Hash 和防御性大小限制。

Gameplay 状态应与已有 `ScriptObjectState` 一起组合为引擎级 Rollback Frame。

## 语言与编辑器集成闭环

结合 Phase 18–24 原生语言能力与引擎集成演进，Gameplay Runtime 已与语言及编辑器层完全打通：

- 源码级 `interface`、继承、虚分派、泛型与可变 `struct`；
- 一等委托、闭包、语言级 `event` 以及宿主跨组件 `connectSignal` 信号连接；
- 源码 `[Attribute]` 自动提取至 `ScriptMetadataRegistry` 与 `InspectorTypeSchema`；
- `sequence` 协程状态机原生支持 `yield wait_ticks`、`yield wait_seconds`、`yield wait_event`、`yield wait_until` 与生命周期自动清理。

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

- `GameplayHost`：实体、计时器、事件、随机流和脚本调用的统一宿主；
- `SceneGameplayDriver`：驱动 `SceneScriptRuntime` 的固定帧入口；
- 命名事件订阅与发布；
- 固定 Tick 的脚本序列，可作为当前阶段的确定性协程模型；
- `ScriptContract`，用于按回调名称和参数数量验证行为、技能、任务和 AI 脚本；
- `ScriptMetadataRegistry`，用于序列化、Inspector、复制和编辑器元数据；
- `installGameplayBindings`，向脚本生成 `CurrentTick`、实体、随机数、计时器和事件 API。

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

## 当前边界

本阶段没有假装完成全部 C# 语法。以下仍属于后续编译器阶段：

- 源码级 `interface`、继承和虚分派；
- 泛型；
- lambda、delegate 和语言级 `event`；
- `for`、`foreach`、`switch`；
- `[Attribute]` 语法；
- `yield`/协程状态机与 `async`；
- `ref`、`out`、装箱和可变 struct receiver。

当前的 Contract、Metadata、Named Event 和 Sequence 是这些语法后续可以直接 Lower 到的稳定运行时能力。

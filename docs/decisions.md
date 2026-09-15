# 决策记录

只记「为什么这么定」，每条一段。改决策时更新对应条目，不追加叙事。起点是 mquickjs-kmp 的同名文档：凡未在此覆盖的条目（平台范围、版本基线、上游锁定方式、原生分发、BCV、只发正式版、Android host test 走宿主 JNI、ref 显式 close、对象过桥方式由调用点选择、JsRuntime 互斥来自 Mutex 等）**原样沿用**。

## 定位：QuickJS 的 KMP 绑定，为 TinyUI 服务，但不只为它

TinyUI（ADR-005）需要 ES2025 + 原生 ESM + 微任务 + 可预编译的引擎，MicroQuickJS 给不了。本 SDK 是 bellard/quickjs 的通用 KMP 绑定，API 面按 TinyUI 的需要排优先级，但不含任何 TinyUI 语义（节点、patch、组件都在 TinyUI 的 `compose/`）。

## 命名：产物 `quickjs-kmp`，主模块 `library`，包 `wang.harlon.quickjs`

与 mquickjs-kmp 平行；`kmp` 同样表示「QuickJS 的 Kotlin 移植」。仓库 `HarlonWang/quickjs-kmp`。不叫 `quickjs-wrapper-kmp`：quickjs-wrapper 是 Android 专用、JNI 直传 JSValue 的另一套设计，两者不是移植关系。

## 上游：bellard/quickjs 本尊，git subtree 锁 commit `04be246`（版本 2026-06-04）

不选 quickjs-ng：TinyUI 只需 ES2025 与本文列出的 API，本尊全有；作者 2024 年起稳定发版；单一上游可追溯。ng 的 API 正在缓慢分化，作为将来可切换选项——shim 的 API 面很小。上游目录 `native/quickjs/`（bellard 仓无 tag，锁 commit 记在 `native/UPSTREAM`，同 mquickjs-kmp）。

## 编译范围：只编引擎核心，不链 `quickjs-libc`

`quickjs.c` / `libregexp.c` / `libunicode.c` / `cutils.c` / `dtoa.c` 五个文件；不要 `std` / `os` 模块、文件 IO、`qjs` 的 REPL。`console.log`、定时器、模块加载全部由宿主经 shim 提供——TinyUI 正是这么要的，通用用户也应如此（引擎不该自己碰文件系统）。`CONFIG_VERSION` 从上游 `VERSION` 文件读入编译宏。

## Kotlin 公共 API 与 mquickjs-kmp 同形

`JsEngine` / `JsRuntime` / `JsRef` / `JsValue` / `ObjectTransport` / `JsBytecode` / `JsProgram` / `registerFunction` / `stats` 等签名保持一致，mquickjs-kmp 的 commonTest 用例集直接搬来当验收。目的：TinyUI 对引擎的依赖面天然是同一组签名，将来抽引擎接口零成本；用户在两个 SDK 间迁移只改坐标。QuickJS 独有能力（模块、Promise 结果、内存限额）作为**新增**而非改形。

## 句柄表保留，内部存 dup 过的 JSValue

QuickJS 对象不移动，但仍不让 Kotlin 持有 JSValue：引用计数要求「拿了就要还」而 Kotlin GC 不保证 finalizer；JNI 传不了结构体；线程约束；Runtime 销毁后无法通知持有者。句柄 = 32 位 generation + 32 位槽位下标，槽位存 `JS_DupValue` 后的值与 Kotlin 侧计数，`release` 时 `JS_FreeValue` 并 generation +1（旧句柄命中复用槽位时因 generation 不匹配而报错，而非读错对象）。`stats().liveRefs` = 各槽计数之和，泄漏可测。宿主函数回调的对象参数为 transient（回调返回自动释放），`retain()` 升为独立引用。与 mquickjs-kmp 的差别只在槽位存 JSValue 而非 GC 根，Kotlin 层语义一字不变。

## 跨界值：`kmpjs_value` 八种 tag 原样保留

undefined / null / bool / number / string / object（JSON 文本）/ exception（message + stack）/ ref。JSON 用 `JS_JSONStringify` / `JS_ParseJSON`；异常栈取 `stack` 属性。ABI 不动是「API 同形」的物质基础。

## 宿主函数：单一 trampoline，`JS_NewCFunctionMagic` 以 magic 携带 fn_id

同 mquickjs-kmp 的分发方式，QuickJS 原生支持 magic 参数，不需要 data 闭包。

## 微任务：每次进入引擎返回前排空

`evaluate` / `callFunction` / `evaluateModule` 执行完主体后循环 `JS_ExecutePendingJob` 直到 `JS_IsJobPending` 为假，再返回 Kotlin。理由：QuickJS 有微任务队列但没有事件循环，若不在此处排空，Promise 回调将永远不跑；排空发生在同一次宿主调用内，TinyUI「一次 K 入口 = 一个事务」的边界因此不变。排空受 interrupt 与超时约束（一个无限自我调度的 Promise 链会被看门狗打断）。

## Promise 结果：调用返回 Promise 时可取最终值

`callFunction` / `evaluateModule` 的结果若为 Promise，排空微任务后用 `JS_PromiseState` / `JS_PromiseResult` 取值：fulfilled 返回值，rejected 抛 `JsException`，pending（等待宿主异步能力）返回 `JsValue.Promise` 的 ref 由调用方持有。业务 `async` 函数被宿主调用时 Kotlin 因此能拿到最终值。

## 模块：原生 ESM，引擎侧只解析预注册的名字

`registerModule(name, bytecode)` 把预编译模块登记进 C 侧名字表；`JS_SetModuleLoaderFunc` 的 loader 只查表，查不到抛 `ReferenceError`；normalize 不处理相对路径（裸说明符原样返回）。`evaluateModule(bytecode)` 求值并按上一条取 Promise 结果，返回 namespace 的 ref。不做文件系统 loader、不做相对路径解析——构建工具负责让模块图里只剩表内名字。Kotlin 回调式 loader（动态取源码）是热下发那一期的事，届时作为新增 API 加入，不改现有形态。模块名即说明符，`import.meta.url` 为 `<scheme>:<name>`，scheme 由 `JsEngineConfig` 指定。

## 字节码：QuickJS 序列化 + 自家文件头

脚本与模块都经 `JS_Eval(COMPILE_ONLY)` → `JS_WriteObject(JS_WRITE_OBJ_BYTECODE)`；加载走 `JS_ReadObject` + `JS_EvalFunction`。保留 mquickjs-kmp 的文件头（绑定上游 commit 与字长）：QuickJS 自带 BC 版本号但不校验 commit；字长是否影响格式待 armeabi-v7a 实测，头里先留字段。「每引擎一个程序」的限制不复存在，Kotlin 层删除对应检查。宿主编译工具 `qjsc-kmp`（`buildHostTools`）与 `JsBytecode.compile` 共用 shim 的编译入口。

## Runtime / Context：一个 `JsEngine` = 一个 Runtime + 一个 Context

不暴露多 Context：TinyUI 每页一 Runtime（限额、中断都是 Runtime 级），通用用户要隔离也应建新引擎。`JsEngineConfig` 暴露 `memoryLimit` / `maxStackSize` / `gcThreshold`；`stats()` 映射 `JS_ComputeMemoryUsage`。

## 取消 / 超时：`JS_SetInterruptHandler` 轮询原子标志

同 mquickjs-kmp。排空微任务阶段同样受其约束。

## 三端构建：沿用 mquickjs-kmp 的 Gradle 驱动 CMake

`-Os -fvisibility=hidden` + strip，`-D_GNU_SOURCE`；Android arm64-v8a / armeabi-v7a / x86_64，iOS device + simulator arm64，macosArm64 作为调试宿主并随包发布；ASan 冒烟沿用 `native/test/shim_test.c` 的做法。预期每架构约 1.2 MB。

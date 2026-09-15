# 决策记录

只记「为什么这么定」，每条一段。改决策时更新对应条目，不追加叙事。起点是 mquickjs-kmp 的同名文档：凡未在此覆盖的条目（平台范围、版本基线、上游锁定方式、原生分发、BCV、只发正式版、Android host test 走宿主 JNI、ref 显式 close、对象过桥方式由调用点选择等）**原样沿用**；「JsRuntime 互斥来自 Mutex」沿用但有 QuickJS 特有的补充，见「取消 / 超时 / 线程」条。mquickjs-kmp 已于 2026-09-15 归档，本仓是它的后继：沿用条目的真值从此在本仓，修复与决策都不再回流。

## 定位：QuickJS 的 KMP 绑定，为 TinyUI 服务，但不只为它

TinyUI（ADR-005）需要 ES2025 + 原生 ESM + 微任务 + 可预编译的引擎，MicroQuickJS 给不了。本 SDK 是 bellard/quickjs 的通用 KMP 绑定，API 面按 TinyUI 的需要排优先级，但不含任何 TinyUI 语义（节点、patch、组件都在 TinyUI 的 `compose/`）。

## 命名：产物 `quickjs-kmp`，主模块 `library`，包 `wang.harlon.quickjs`

与 mquickjs-kmp 平行；`kmp` 同样表示「QuickJS 的 Kotlin 移植」。仓库 `HarlonWang/quickjs-kmp`。不叫 `quickjs-wrapper-kmp`：quickjs-wrapper 是 Android 专用、JNI 直传 JSValue 的另一套设计，两者不是移植关系。

## 上游：bellard/quickjs 本尊，git subtree 锁 commit `04be246`（版本 2026-06-04）

不选 quickjs-ng：TinyUI 只需 ES2025 与本文列出的 API，本尊全有；作者 2024 年起稳定发版；单一上游可追溯。ng 的 API 正在缓慢分化，作为将来可切换选项——shim 的 API 面很小。上游目录 `native/quickjs/`（bellard 仓无 tag，锁 commit 记在 `native/UPSTREAM`，同 mquickjs-kmp）。

## 编译范围：只编引擎核心，不链 `quickjs-libc`；`console` 与 `performance.now` 由 shim 提供

`quickjs.c` / `libregexp.c` / `libunicode.c` / `cutils.c` / `dtoa.c` 五个文件；不要 `std` / `os` 模块、文件 IO、`qjs` 的 REPL。定时器、模块加载全部由宿主经 shim 提供——TinyUI 正是这么要的，通用用户也应如此（引擎不该自己碰文件系统）。引擎核心没有 `console` 对象也没有 `performance`（两者都在 libc 里），shim 自己挂 `console.log`（走 `JsEngineConfig.logger`）与 `performance.now`（单调时钟），这是 shim 的义务而非可选项：TinyUI 的 bench 与 mquickjs-kmp 的验收用例都依赖它们。`CONFIG_VERSION` 从上游 `VERSION` 文件读入编译宏。

## Kotlin 公共 API 以 mquickjs-kmp 为起点，不再与之同形

`JsEngine` / `JsRuntime` / `JsRef` / `JsValue` / `ObjectTransport` / `registerFunction` / `stats` 等签名从 mquickjs-kmp 搬来，它的 commonTest 用例集直接搬来当验收，这样骨架与用例零成本复用。mquickjs-kmp 归档后「两个 SDK 间迁移只改坐标」的目标不复存在，同形不再是约束：本仓 API 按 QuickJS 与 TinyUI 的需要独立演进，签名可以改形。已经落地的差异：`JsEngineConfig` 换成 `memoryLimit` / `maxStackSize` / `gcThreshold`；凡是把 MicroQuickJS 的限制暴露成公共 API 的部分（字节码字长、每引擎一个程序、加载顺序）一律删除，对应用例作废或反转，清单见 roadmap.md 的验收基线。

## 句柄表保留，内部存 dup 过的 JSValue

QuickJS 对象不移动，但仍不让 Kotlin 持有 JSValue：引用计数要求「拿了就要还」而 Kotlin GC 不保证 finalizer；JNI 传不了结构体；线程约束；Runtime 销毁后无法通知持有者。句柄 = 32 位 generation + 32 位槽位下标，槽位存 `JS_DupValue` 后的值与 Kotlin 侧计数，`release` 时 `JS_FreeValue` 并 generation +1（旧句柄命中复用槽位时因 generation 不匹配而报错，而非读错对象）。槽位表是连续数组、按需 realloc：QuickJS 没有 MicroQuickJS 那种侵入式 GC 链表，不需要逐槽 malloc。`stats().liveRefs` = 各槽计数之和，泄漏可测。宿主函数回调的对象参数为 transient（回调返回自动释放），`retain()` 升为独立引用。

`JS_FreeRuntime` 断言 GC 对象链表为空，shim 里任何一个没 free 的 JSValue 都会让引擎 close 命中 assert；三端构建不定义 `NDEBUG`，assert 在发布包里也生效。所以 close 是一条固定顺序：释放全部槽位 → 清模块名字表 → 丢弃 pending job → `JS_FreeContext` → `JS_FreeRuntime`。「持有未 close 的 JsRef 时 close 引擎」是 ASan 冒烟的必备用例，让这条顺序可测而不是靠记忆。ref 泄漏的上界仍是引擎本身：引擎 close 时槽位全部释放。

## 跨界值：`kmpjs_value` 的 tag 原样保留，为 BigInt 与二进制预留

undefined / null / bool / number / string / object（JSON 文本）/ exception（message + stack）/ ref 八种 tag 沿用。JSON 用 `JS_JSONStringify` / `JS_ParseJSON`；异常栈取 `stack` 属性。ABI 不动是「API 同形」的物质基础。

ES2025 下 JSON 过桥的损失面比 ES5 大得多，这是默认 `ObjectTransport.JSON` 的已知代价，写明而不隐藏：Map / Set 序列化为 `{}`，Date 变 ISO 字符串，BigInt 抛 TypeError，ArrayBuffer / TypedArray 变 `{}`，`undefined` 属性与函数属性丢失，循环引用抛错。需要保真的场景用 `ObjectTransport.REF`。在此之上新增两个 tag：BIGINT（十进制文本携带）与 BINARY（ArrayBuffer / TypedArray 内容深拷贝成字节，同 quickjs-wrapper 的做法），Kotlin 侧对应 `JsValue.BigInt` 与 `JsValue.Bytes`。`JsValue` 是 sealed，子类集合在 0.1.0 前定完，之后新增就是破坏性变更。

## 宿主函数：单一 trampoline，`JS_NewCFunctionMagic` 以 magic 携带 fn_id

同 mquickjs-kmp 的分发方式，QuickJS 原生支持 magic 参数，不需要 data 闭包。

## 微任务：最外层宿主调用返回前排空

`evaluate` / `callFunction` / `evaluateModule` 以及 ref 的属性读取执行完主体后循环 `JS_ExecutePendingJob` 直到队列为空，再返回 Kotlin。理由：QuickJS 有微任务队列但没有事件循环，若不在此处排空，Promise 回调将永远不跑；排空发生在同一次宿主调用内，TinyUI「一次 K 入口 = 一个事务」的边界因此不变。排空只发生在**最外层**：shim 已有的 idle / running 状态机就是深度判断，宿主函数里再次进入引擎（嵌套 `callFunction`）的内层返回时不排空，否则会把外层脚本执行中途的微任务提前跑掉，违反「微任务在当前宏任务结束后执行」的语义。排空受 interrupt 与超时约束（一个无限自我调度的 Promise 链会被看门狗打断）。

`JS_ExecutePendingJob` 返回 -1 时停止排空，把异常作为本次调用的 `JsException` 抛出（调用主体自己的异常优先，先于排空捕获）：实践中只有中断与 OOM 这类不可捕获异常会走到这里，反应回调里的普通异常由引擎转成派生 Promise 的 rejection，归下一段。排空被打断后队列里剩下的 job **丢弃**，而不是留到下一次调用：无限链留着会让之后每一次调用都挂住，引擎等于报废。QuickJS 没有清空队列的 API，丢弃的做法是把栈限额临时设为 1 字节再把队列跑空，每个 job 在函数入口就以栈溢出失败、来不及再入队，跑完恢复限额，期间产生的 rejection 一并忽略。

排空后的未处理 rejection 经 `JS_SetHostPromiseRejectionTracker` 收集，回调 `JsEngineConfig.onUnhandledRejection`；未设置时经 `logger` 输出一行。不默认抛 `JsException`：本次调用的返回值已经算出来了，fire-and-forget 的 `async` 失败不该吞掉它。TinyUI 把 handler 接进错误 sink。

## Promise 结果：调用返回 Promise 时可取最终值

`evaluate` / `callFunction` / `evaluateModule` 的结果若为 Promise，排空微任务后用 `JS_PromiseState` / `JS_PromiseResult` 取值：fulfilled 返回值，rejected 抛 `JsException`（并从未处理 rejection 清单里移除，不重复上报），pending（等待宿主异步能力）返回带 `isPromise` 的 `JsRef`，**不论调用点选的是 JSON 还是 REF**：JSON 化一个 pending Promise 只能得到 `{}`，调用方要的是之后还能拿到结果的把手。业务 `async` 函数被宿主调用时 Kotlin 因此能拿到最终值。属性读取（`JsRef.get`）不解包，读到什么给什么。

## 模块：原生 ESM，引擎侧只解析预注册的名字

`registerModule(name, bytecode)` 把预编译模块登记进 C 侧名字表；`JS_SetModuleLoaderFunc` 的 loader 只查表，查不到抛 `ReferenceError`；normalize 不处理相对路径（裸说明符原样返回）。`evaluateModule(bytecode)` 求值并按上一条取 Promise 结果，返回 namespace 的 ref。不做文件系统 loader、不做相对路径解析——构建工具负责让模块图里只剩表内名字。Kotlin 回调式 loader（动态取源码）是热下发那一期的事，届时作为新增 API 加入，不改现有形态。模块名即说明符，`import.meta.url` 为 `<scheme>:<name>`，scheme 由 `JsEngineConfig` 指定。

## 字节码：QuickJS 序列化 + 自家文件头，字长无关

脚本与模块都经 `JS_Eval(COMPILE_ONLY)` → `JS_WriteObject(JS_WRITE_OBJ_BYTECODE)`；加载走 `JS_ReadObject` + `JS_EvalFunction`。文件头只记 magic 与上游 commit：QuickJS 自带 BC 版本号但不校验 commit。不记字长：QuickJS 的序列化全部是 leb128 或定宽小端，BigInt limb 虽按 `JS_LIMB_BITS` 分 32 / 64 位写但字节序列相同，唯一写指针的 `JS_WRITE_OBJ_SAB` 不开，所以同一份字节码 arm64 与 armeabi-v7a 通用（qjsc 只有字节序开关、没有字长开关，同一事实）。`JsBytecode.wordSize` 与 `compile` 的 `wordSize` 参数不存在。调试信息裁剪对应 QuickJS 的 Runtime 级 `JS_SetStripInfo`，语义是「去源码」或「去全部调试信息」而非「去列号」，`compile` 的参数为 `strip: JsBytecode.Strip`（NONE / SOURCE / DEBUG）。「每引擎一个程序」「加载必须先于求值」的限制不复存在，可以多次加载、任意时机加载。宿主编译工具 `qjsc-kmp`（`buildHostTools`）与 `JsBytecode.compile` 共用 shim 的编译入口。

## Runtime / Context：一个 `JsEngine` = 一个 Runtime + 一个 Context

不暴露多 Context：TinyUI 每页一 Runtime（限额、中断都是 Runtime 级），通用用户要隔离也应建新引擎。`JsEngineConfig` 暴露 `memoryLimit` / `maxStackSize` / `gcThreshold`；`stats()` 映射 `JS_ComputeMemoryUsage`。`maxStackSize` 默认 256 KB 而不是 QuickJS 自带的 1 MB：栈溢出检查只在限额小于线程实际栈时才起作用，Apple 非主线程与 Kotlin/Native worker 默认栈 512 KB，用 1 MB 等于没检查，深递归直接段错误而非 `RangeError`。文档写明「必须小于运行 JsRuntime 的线程栈」。

## 取消 / 超时 / 线程：`JS_SetInterruptHandler` 轮询原子标志，每次入口先 `JS_UpdateStackTop`

中断同 mquickjs-kmp。QuickJS 的中断异常是不可捕获的（`JS_SetUncatchableException`），JS 里的 `try / catch` 拦不住，比 MicroQuickJS 强，用例要覆盖「catch 块内仍能被中断」。排空微任务阶段同样受其约束。

互斥仍由 `JsRuntime` 的 Mutex 保证、dispatcher 只决定在哪跑，但 QuickJS 多一条：栈溢出检查基于 `rt->stack_top`，这个值在 `JS_NewRuntime` 时取创建线程的栈指针，换线程后要么误报溢出、要么检查失效。`limitedParallelism(1)` 只保证串行不保证同线程，所以 shim 的每个入口（eval、call、ref 操作、排空）第一行调 `JS_UpdateStackTop`。

## 三端构建：沿用 mquickjs-kmp 的 Gradle 驱动 CMake

`-Os -fvisibility=hidden` + strip，`-D_GNU_SOURCE`；Android arm64-v8a / armeabi-v7a / x86_64，iOS device + simulator arm64，macosArm64 作为调试宿主并随包发布；ASan 冒烟沿用 `native/test/shim_test.c` 的做法。预期每架构约 1.2 MB。

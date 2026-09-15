# 路线图

验收基线：mquickjs-kmp 的 commonTest 用例集（两端各 60 例）搬来后全绿，再加 QuickJS 独有能力的用例。搬来的用例中四条描述的是 MicroQuickJS 的限制，直接删除：`oneProgramPerEngine`、`loadingAfterScriptRanIsRejected`、`loadingAfterRegisterFunctionIsRejected`、`wrongWordSizeIsRejected`；前三条反转为「多次加载、任意时机加载」用例。

## M1 打通链路

- subtree 锁定 bellard/quickjs `04be246`，`native/UPSTREAM` 记 commit
- shim：`kmpjs_value` 跨界、trampoline 宿主函数、`kmpjs_eval` / `kmpjs_call`、异常与栈；每个入口先 `JS_UpdateStackTop`
- shim 挂 `console.log`（走 logger）与 `performance.now`
- CMake 三端构建，JNI 与 cinterop 两套 actual，`buildNativeHostJni`
- macOS 与 Android 宿主 JNI 测试跑通 mquickjs-kmp 的 M1 用例，加一条跨线程调用用例
- 与 mquickjs-kmp 的差异点在此阶段落实：不链 `quickjs-libc`、无 stdlib 派生、无宿主生成工具；`JS_Eval` / `JS_ParseJSON` 同样要求 NUL 结尾

## M2 核心 API

- 句柄表（连续数组、dup / free 版本）、`JsRef`、`ObjectTransport`、transient / retain；引擎 close 的固定顺序，ASan 用例「持有未 close 的 JsRef 时 close 引擎」
- ~~新增 tag：BIGINT、BINARY；`JsValue.BigInt` / `JsValue.Bytes`~~ 已完成
- `JsRuntime`：Mutex、dispatcher、interrupt、超时；`JsEngineConfig` 的 `memoryLimit` / `maxStackSize`（默认 256 KB）/ `gcThreshold`；用例「catch 块内仍能被中断」
- ~~**微任务排空**（只在最外层）与 **Promise 结果**~~ 已完成，含中断后丢弃残留 job
- ~~未处理 rejection：`JS_SetHostPromiseRejectionTracker` → `JsEngineConfig.onUnhandledRejection`~~ 已完成
- **模块**：`registerModule` 名字表、loader 回调、`evaluateModule`；新增模块用例（import 命中表、命中失败、顶层 await、namespace 访问）
- `stats()` 映射 `JS_ComputeMemoryUsage`；`JsRefLeakTest`

## M3 字节码、工具与发布

- 脚本与模块字节码：`JsBytecode.compile`（script / module 两种模式、`strip` 三档）、文件头（magic + commit，无字长）、`JsEngine.loadBytecode` / `registerModule(bytecode)`；同一份字节码在 arm64 与 armeabi-v7a 上加载的用例
- 宿主工具 `qjsc-kmp`（`buildHostTools`）
- ASan 冒烟挂 check / CI；build.yml / publish.yml；BCV klib dump
- 发 `wang.harlon:quickjs-kmp:0.1.0`
- 验收：TinyUI `bench/signal.js` 经 `JsEngine` 跑通 S1～S4

## 之后

- Kotlin 回调式 module loader（热下发）
- 多 Context（若有隔离但共享堆的需求）
- quickjs-ng 切换评估

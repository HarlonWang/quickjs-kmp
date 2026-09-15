# 路线图

验收基线：mquickjs-kmp 的 commonTest 用例集（两端各 60 例）搬来后全绿，再加 QuickJS 独有能力的用例。

## M1 打通链路

- subtree 锁定 bellard/quickjs `04be246`，`native/UPSTREAM` 记 commit
- shim：`kmpjs_value` 跨界、trampoline 宿主函数、`kmpjs_eval` / `kmpjs_call`、异常与栈
- CMake 三端构建，JNI 与 cinterop 两套 actual，`buildNativeHostJni`
- macOS 与 Android 宿主 JNI 测试跑通 mquickjs-kmp 的 M1 用例
- 与 mquickjs-kmp 的差异点在此阶段落实：不链 `quickjs-libc`、无 stdlib 派生、无 NUL 结尾要求

## M2 核心 API

- 句柄表（dup / free 版本）、`JsRef`、`ObjectTransport`、transient / retain
- `JsRuntime`：Mutex、dispatcher、interrupt、超时；`JsEngineConfig` 的 `memoryLimit` / `maxStackSize` / `gcThreshold`
- **微任务排空**与 **Promise 结果**：`callFunction` 返回 Promise 时取最终值；新增 `async` 函数用例
- **模块**：`registerModule` 名字表、loader 回调、`evaluateModule`；新增模块用例（import 命中表、命中失败、顶层 await、namespace 访问）
- `stats()` 映射 `JS_ComputeMemoryUsage`；`JsRefLeakTest`

## M3 字节码、工具与发布

- 脚本与模块字节码：`JsBytecode.compile`（script / module 两种模式）、文件头、`JsEngine.loadBytecode` / `registerModule(bytecode)`
- 宿主工具 `qjsc-kmp`（`buildHostTools`），armeabi-v7a 字长实测
- ASan 冒烟挂 check / CI；build.yml / publish.yml；BCV klib dump
- 发 `wang.harlon:quickjs-kmp:0.1.0`
- 验收：TinyUI `bench/signal.js` 经 `JsEngine` 跑通 S1～S4

## 之后

- Kotlin 回调式 module loader（热下发）
- 多 Context（若有隔离但共享堆的需求）
- quickjs-ng 切换评估

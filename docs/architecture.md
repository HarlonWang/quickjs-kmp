# 架构

QuickJS 的对象不移动，但 `JSValue` 是引用计数的：拿到就要还，而且只能在创建它的 Runtime 存活期间还。Kotlin 的 GC 不保证 finalizer 何时跑、JNI 传不了结构体，所以绑定层的设计仍然是**Kotlin 侧永远不持有原生 `JSValue`**，与 mquickjs-kmp 一致；差别只在 C 层怎么留住对象。

## 分层

```
commonMain      JsEngine / JsValue / JsRef / JsRuntime / JsException（expect 声明 + 纯 Kotlin 逻辑）
    │
    ├── native/shim     quickjs_kmp.c/.h：句柄化 C API，Android 与 Apple 共用同一份
    │
    ├── androidMain     JNI（native/jni）→ shim
    └── appleMain       cinterop → shim
```

平台绑定只对接 shim，不直接对接 `quickjs.h`。理由：JNI 与 cinterop 只需各写一次同样的薄封装；引用计数的正确性被封在 C 层；shim 接口只用整数句柄与 UTF-8 字节，跨语言最省事。

## shim 的设计约束

**值跨界只传原始类型与字节。** `kmpjs_value` 是唯一的跨界类型：undefined / null / bool / number 直接携带，字符串以 UTF-8 字节加长度传递，BigInt 以十进制文本传递，ArrayBuffer / TypedArray 以字节拷贝传递（判型靠启动时探出的 class id 区间），对象与数组经 `JS_JSONStringify` 以 JSON 文本传递（函数的 JSON 为空）。宿主函数的返回值反向走 `JS_ParseJSON`。字符串以字节数组过桥，Kotlin 侧用自带的 `Wtf8` 编解码：引擎对未配对代理项按 WTF-8 输出并能解回，JNI 的 modified UTF-8 与 Kotlin 标准 UTF-8 编解码都会把它们改写成替换字符。`JS_Eval` 与 `JS_ParseJSON` 要求输入以 NUL 结尾，shim 把所有源码与 JSON 拷贝成 NUL 结尾再交给引擎。

**单一 trampoline 承接宿主函数。** `registerFunction` 用 `JS_NewCFunctionMagic` 造一个 C 函数对象挂到全局对象上，magic 里打包 fn_id 与 flags，调用时 C 侧按 fn_id 分发到 Kotlin。Kotlin/Native 的 `staticCFunction` 不能捕获状态，引擎实例通过 `JS_SetContextOpaque` 挂在上下文上，回调用 `StableRef` 取回。

**句柄表取代 JSValue。** 需要让 Kotlin 长期持有 JS 对象时（`ObjectTransport.REF`），对象 `JS_DupValue` 后存进引擎的 slot 表，Kotlin 只拿整数 ref。slot 表是连续数组，每个 slot 带引用计数：传给宿主函数的 ref 计数为 1 且在调用返回后释放，`retain` 加一后归调用方。ref 是 64 位整数，低 32 位是 slot 下标、高 32 位是 generation，slot 复用时 generation 递增，过期句柄被拒绝而不会碰到别的对象；Kotlin 侧的 `JsRef` 在 close 或回调结束后也标记失效。属性读写与 `toJSON` 都可能执行脚本，所以每个 ref 操作和求值一样进入 running 态，超时能中断 accessor 里的死循环。

**引擎 close 有固定顺序。** `JS_FreeRuntime` 断言 GC 对象链表为空，三端构建不定义 `NDEBUG`，shim 里任何一个没 free 的 `JSValue` 都会让 close 直接 abort。顺序是：释放全部 slot → `JS_FreeContext` → `JS_FreeRuntime`。C 测试的最后一步就是带着未释放的 ref 销毁引擎。

**微任务在最外层调用返回前排空。** `run_begin` 判定的最外层入口在主体执行完后循环 `JS_ExecutePendingJob`，随后解包 Promise 结果、上报本次调用里未处理的 rejection，再把结果转成 `kmpjs_value`。调用主体自己的异常先于排空捕获，job 抛出的不可捕获异常（中断、OOM）才会取代结果。排空被打断时剩余 job 会被丢弃（临时 1 字节栈限额跑空队列），引擎因此不会被无限链拖死。rejection 经 `JS_SetHostPromiseRejectionTracker` 记入清单，同一 tick 里后来挂上 handler 的会被引擎再次通知并从清单移除，所以清单只在排空之后判定。

**模块只认名字。** `registerModule` 存源码，`JS_SetModuleLoaderFunc` 的 loader 在第一次 import 时查表编译并设 `import.meta.url`，normalize 原样返回说明符。表里没有的名字交给 `kmpjs_module_fn`（`JsEngineConfig.moduleLoader`）同步取源码或字节码，取到的模块同样占名进表；字节码先在裸 context 里核对编译名等于请求名再读进引擎，理由同 `registerModule(bytecode)`。`evaluateModule` 先 COMPILE_ONLY 记下 `JSModuleDef*` 再 `JS_EvalFunction`，得到的 Promise 走 `finish` 的排空与解包，fulfilled 时用记下的 `JSModuleDef*` 取 namespace 代替 Promise 的值。

**字节码是引擎序列化加一个头。** `kmpjs_compile` 在临时 Runtime 里 `JS_Eval(COMPILE_ONLY)` 再 `JS_WriteObject`，前置 52 字节头（magic、种类、上游 commit）；`kmpjs_run_bytecode` 核对头后 `JS_ReadObject`，脚本直接 `JS_EvalFunction`，模块先 `JS_ResolveModule` 再走与 `kmpjs_eval_module` 相同的收尾。字节码模块按编译时的名字注册；名字先在裸 context 里读出来做查重，再读进引擎，因为读入引擎即入缓存、无法撤销。

**引擎核心没有 `console`。** `console.log`、`print`、`performance.now` 由 shim 挂到全局对象上（`quickjs-libc` 不链接），`console.log` 走 `JsEngineConfig.logger`，非字符串参数用引擎的 `JS_PrintValue` 格式化。

## 运行时约束

- 上下文单线程。`JsEngine` 不做同步，调用方在单线程使用或自行串行化；`interrupt()` 是唯一可跨线程调用的成员。`JsRuntime` 用内部 `Mutex` 保证独占，工作在传入的 dispatcher 上跑，默认 `Dispatchers.Default` 单车道；协程取消与 `withTimeout` 通过一个看门狗协程转成 `interrupt()`，`shutdown` 先中断再在锁下关闭。
- 换线程是允许的，但 QuickJS 的栈溢出检查以 `JS_NewRuntime` 时的栈指针为基准，所以 shim 在每次最外层入口先 `JS_UpdateStackTop`。`maxStackSize` 必须小于线程实际的栈，否则检查失效、深递归直接段错误。
- 内存上限来自 `JS_SetMemoryLimit`。撞到上限时引擎连 Error 对象都分配不出来，会抛 `null`；shim 的分配器在拒绝分配时置一个标志，`exception_to_out` 据此把这个 `null` 报成 `InternalError: out of memory`，与脚本自己 `throw null` 区分开。OOM 与 JS 异常统一经 `JS_GetException` 取 message（`toString()` 结果）与 `stack`，映射为 `JsException`。
- 中断走 `JS_SetInterruptHandler`，引擎状态是 C11 `atomic_int` 的 idle / running / interrupted 三态机：求值开始 CAS 进入 running，`interrupt()` 只在 running 时 CAS 成 interrupted，空闲时调用是 no-op，嵌套求值不改状态。QuickJS 的中断异常是不可捕获的，JS 里的 `try / catch` 拦不住；脚本以 `InternalError: interrupted` 终止，引擎随后可继续使用。
- 宿主回调（宿主函数、logger）里的 Kotlin 异常一律在回调内截住：Kotlin/Native 异常越过 C 边界会终止进程。宿主函数异常转成 JS `Error`，logger 异常吞掉。

## 公共 API 边界

core 暴露 `JsEngine`、`JsEngineConfig`、`JsValue`（sealed）、`JsRef`、`ObjectTransport`、`JsHostFunction`、`JsException`、`JsRuntime`、`JsEngineStats`、`QuickJs`。类型化桥接（kotlinx.serialization）是对这些类型的扩展函数（`*Serialization.kt`），不动核心类型。公共 API 由 binary-compatibility-validator 守门，`api/` 目录下的 `.api` 文件入库。

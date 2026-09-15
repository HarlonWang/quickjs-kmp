# quickjs-kmp

> [QuickJS](https://github.com/bellard/quickjs) 的 Kotlin Multiplatform 绑定。QuickJS 是 Fabrice Bellard 的 ES2025 JavaScript 引擎。Android + iOS，macOS 作为调试宿主。

[![Platform](https://img.shields.io/badge/Platform-Android%20%7C%20iOS-brightgreen)](https://kotlinlang.org/docs/multiplatform.html)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

[English](./README.md) | 中文

> 尚未发布。[mquickjs-kmp](https://github.com/HarlonWang/mquickjs-kmp)（已归档）的后继，已完成与后续计划见 [docs/roadmap.md](docs/roadmap.md)。为 [TinyUI](https://github.com/HarlonWang/tinyui) 而建，但不绑定它。

## 平台

| 目标 | 绑定 | 说明 |
| --- | --- | --- |
| Android（`minSdk 24`） | JNI | `.so` 内置于 AAR（`arm64-v8a`、`armeabi-v7a`、`x86_64`） |
| iOS（`iosArm64`、`iosSimulatorArm64`） | cinterop | 静态库打进 klib，不需要 CocoaPods / SPM |
| macOS（`macosArm64`） | cinterop | ASan 的调试宿主，同时随包发布 |

## 用法

```kotlin
JsEngine(JsEngineConfig(memoryLimit = 8L * 1024 * 1024, logger = ::println)).use { engine ->
    engine.registerFunction("discount") { args ->
        val amount = (args[0] as JsValue.Num).value
        JsValue.Num(if (amount > 100) amount * 0.9 else amount)
    }
    engine.evaluate("const total = discount(120);")
    engine.evaluate("total")                      // JsValue.Num(108.0)
    engine.evaluate("({ok: total > 100})")        // JsValue.Json("{\"ok\":true}")
    engine.evaluate("console.log('done', total)") // logger 收到 "done 108"
}
```

- 原始类型以 `JsValue.Num` / `Str` / `Bool` / `Null` / `Undefined` 过桥；对象与数组默认以 `JsValue.Json` 过桥。JSON 会丢掉 `JSON.stringify` 本来就丢的东西（函数、`undefined` 属性、`Map` / `Set` 的内容），需要保真时用 `ObjectTransport.REF`。
- 脚本抛异常、语法错误或撞到 `memoryLimit` 时抛出 `JsException`，带引擎的 message 与 `stack`。
- 宿主函数抛出的 Kotlin 异常在 JS 侧表现为带同样 message 的 `Error`。
- `engine.interrupt()` 可在任意线程调用，以不可捕获的 `InternalError: interrupted` 终止正在运行的脚本。
- `JsEngineConfig.maxStackSize`（默认 256 KB）必须小于运行引擎的线程栈。
- 引擎是单线程的；用下面的 `JsRuntime`，或自行串行化访问。

### 持有 JS 对象：`JsRef`

指定 `ObjectTransport.REF`，对象就以句柄而非 JSON 返回。`JsRef` 可以读写属性、按下标访问数组、带 `this` 与参数调用函数，用完必须 close：在此之前对象一直活在引擎里。

```kotlin
JsEngine().use { engine ->
    val rules = engine.evaluate("({limit: 3, check(n) { return n <= this.limit; }})", objects = ObjectTransport.REF) as JsRef
    rules.use { r ->
        r.set("limit", JsValue.Num(10))
        val check = r.get("check", ObjectTransport.REF) as JsRef
        check.use { it.invoke(thisArg = r, args = listOf(JsValue.Num(7))) } // JsValue.Bool(true)
    }
}
```

以 `ObjectTransport.REF` 注册的宿主函数收到的 ref 只在本次调用内有效，`retain()` 可以留住一个。`engine.stats().liveRefs` 报告还有多少 ref 没关，SDK 自己的测试就是靠它证明没有泄漏。

### 类型化的值：kotlinx.serialization

`@Serializable` 类型经 kotlinx.serialization 过桥（运行时随 SDK 提供，编译器插件加在你自己的模块里）：原始类型变成 `JsValue.Num` / `Str` / `Bool` / `Null`，其余变成 JSON 文本。`Json.encodeToJsValue` / `Json.decodeFromJsValue` 是基础件；`JsValue.decode<T>()`、`JsEngine.evaluateAs<T>()` 与一到三参数的类型化 `registerFunction` 是快捷方式。

### 协程：`JsRuntime`

`JsRuntime` 用一把互斥锁串行化对同一个引擎的所有访问，工作跑在你指定的 dispatcher 上（默认 `Dispatchers.Default` 的单车道），并把协程取消与超时映射为引擎 interrupt。

```kotlin
val runtime = JsRuntime()
try {
    try {
        runtime.evaluate("for (;;) {}", timeout = 200.milliseconds)
    } catch (e: TimeoutCancellationException) {
        // 脚本已被中断；引擎仍可继续使用
    }
    runtime.withEngine { evaluate("1 + 1") } // 独占访问，块内可以使用 ref
} finally {
    runtime.shutdown()
}
```

## 文档

- [docs/architecture.md](docs/architecture.md)：分层、句柄表、宿主函数 trampoline、线程
- [docs/native-build.md](docs/native-build.md)：上游引入与各目标构建
- [docs/decisions.md](docs/decisions.md)：为什么这么命名、这么划范围
- [docs/roadmap.md](docs/roadmap.md)：里程碑

## 构建

- Gradle daemon 用 JDK 25（`gradle/gradle-daemon-jvm.properties`，缺失时 Gradle 自动下载）、Xcode、装有 `gradle/libs.versions.toml` 里锁定的 NDK 版本的 Android SDK、PATH 上有 `cmake`。
- `./gradlew :library:macosArm64Test` 是最快的完整检查；`:library:testAndroidHostTest` 在宿主上经真实 JNI 桥跑同一套用例；`:library:connectedAndroidDeviceTest` 在设备或模拟器上跑。
- `./gradlew :library:nativeShimTest` 在 AddressSanitizer 下跑 C 层的 shim 测试。
- CI（`.github/workflows/build.yml`）在每个 PR 与推到 `main` 时跑 shim 测试、macOS 测试、Android 宿主测试、iOS 编译、Android AAR 打包与 API 检查；`publish.yml` 在推版本 tag 时发布到 Maven Central。

## 上游

引擎以 `git subtree` 引入到 `native/quickjs`，锁定在 `native/UPSTREAM` 记录的 commit，运行时通过 `QuickJs.upstreamCommit` 可查。只编译引擎核心（`quickjs.c`、`libregexp.c`、`libunicode.c`、`cutils.c`、`dtoa.c`），不链接 `quickjs-libc`，所以 `console.log`、`print` 与 `performance.now` 由 shim 提供，其余（定时器、模块加载）由宿主提供。

## 许可证

MIT。QuickJS 本身为 MIT，版权归 Fabrice Bellard 与 Charlie Gordon。

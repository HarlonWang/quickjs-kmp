# quickjs-kmp

> [QuickJS](https://github.com/bellard/quickjs) 的 Kotlin Multiplatform 绑定。QuickJS 是 Fabrice Bellard 的 ES2025 JavaScript 引擎。Android + iOS，macOS 作为调试宿主。

[![Maven Central](https://img.shields.io/maven-central/v/wang.harlon/quickjs-kmp?color=blue&label=Maven%20Central)](https://central.sonatype.com/artifact/wang.harlon/quickjs-kmp)
[![Platform](https://img.shields.io/badge/Platform-Android%20%7C%20iOS-brightgreen)](https://kotlinlang.org/docs/multiplatform.html)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

[English](./README.md) | 中文

> [mquickjs-kmp](https://github.com/HarlonWang/mquickjs-kmp)（已归档）的后继，已完成与后续计划见 [docs/roadmap.md](docs/roadmap.md)。为 [TinyUI](https://github.com/HarlonWang/tinyui) 而建，但不绑定它。

## 平台

| 目标 | 绑定 | 说明 |
| --- | --- | --- |
| Android（`minSdk 24`） | JNI | `.so` 内置于 AAR（`arm64-v8a`、`armeabi-v7a`、`x86_64`） |
| iOS（`iosArm64`、`iosSimulatorArm64`） | cinterop | 静态库打进 klib，不需要 CocoaPods / SPM |
| macOS（`macosArm64`） | cinterop | ASan 的调试宿主，同时随包发布 |

## 安装

```kotlin
commonMain.dependencies {
    implementation("wang.harlon:quickjs-kmp:latest.version")
}
```

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

- 原始类型以 `JsValue.Num` / `Str` / `Bool` / `Null` / `Undefined` 过桥，`BigInt` 以十进制文本装在 `JsValue.BigInt` 里，`ArrayBuffer` 与各类 TypedArray 以字节拷贝装在 `JsValue.Bytes` 里（交给 JS 的 `Bytes` 变成 `ArrayBuffer`）；其余对象与数组默认以 `JsValue.Json` 过桥。JSON 会丢掉 `JSON.stringify` 本来就丢的东西（函数、`undefined` 属性、`Map` / `Set` 的内容），需要保真时用 `ObjectTransport.REF`。
- 脚本抛异常、语法错误或撞到 `memoryLimit` 时抛出 `JsException`，带引擎的 message 与 `stack`。
- 宿主函数抛出的 Kotlin 异常在 JS 侧表现为带同样 message 的 `Error`。
- `engine.interrupt()` 可在任意线程调用，以不可捕获的 `InternalError: interrupted` 终止正在运行的脚本。
- `JsEngineConfig.maxStackSize`（默认 256 KB）必须小于运行引擎的线程栈。
- 引擎是单线程的；用下面的 `JsRuntime`，或自行串行化访问。

### Promise 与 `async`

每次最外层引擎调用返回前都会排空微任务队列，所以 `then` 回调和 `await` 之后的续行在同一次调用里跑完。Promise 结果会被解包：fulfilled 给出它的值，rejected 抛 `JsException`，仍是 pending 的以 `isPromise` 为 true 的 `JsRef` 返回；Promise 本身可能在之后的调用中 settle，标记不会随之改变。

```kotlin
JsEngine(JsEngineConfig(onUnhandledRejection = { e -> println("lost: ${e.message}") })).use { engine ->
    engine.evaluate("async function total(a, b) { await null; return a + b; }")
    engine.evaluate("total(1, 2)")                       // JsValue.Num(3.0)
    engine.evaluate("Promise.reject(new Error('x'))")    // 抛 JsException("Error: x")
    engine.evaluate("Promise.reject(new Error('y')); 0") // 返回 0，handler 收到 "Error: y"
}
```

调用返回时仍没人处理的 rejection 交给 `onUnhandledRejection`，没设 handler 时经 `logger` 输出一行。中断一次调用也会丢弃它留下的微任务。

### ES 模块

模块按脚本里写的说明符原文查名字表：先注册页面可能 import 的源码，再求值页面模块、读它的 namespace。没有文件系统 loader，也不解析相对路径，构建工具要把模块图打平成裸说明符。

```kotlin
JsEngine(JsEngineConfig(moduleScheme = "app")).use { engine ->
    engine.registerModule("util", "export const url = import.meta.url; export function twice(n) { return n * 2; }")
    engine.evaluateModule("import { twice, url } from 'util'; export default twice(21); export const from = url;", name = "main").use { ns ->
        ns.get("default") // JsValue.Num(42.0)
        ns.get("from")    // JsValue.Str("app:util")
    }
}
```

`evaluateModule` 以 `JsRef` 返回 namespace，之后该模块可以按名字被 import。注册的模块在第一次 import 时编译、只执行一次；注册名与求值名共用一个命名空间，任何名字占用两次都会抛异常（尖括号形式的名字如默认的 `<module>` 视为匿名）。支持顶层 `await`：调用返回时仍未完成的模块以带 `isPromise` 的 `JsRef` 返回。每次 `evaluateModule` 都会把编译后的模块留在引擎里直到引擎关闭。

名字表查不到的名字可以交给 `JsEngineConfig.moduleLoader`：在第一次 import（静态或动态 `import()`）时问它，回答 `JsModuleSource.Text` 或 `JsModuleSource.Bytecode`（必须以该名字编译）或 `null` 表示不存在；给出的模块进缓存、不再问，`null` 或抛异常不占名，下次 import 同名会再问。已注册的名字不会问到它。loader 在引擎线程上、在发起 import 的那次调用内同步执行，不能回调引擎，所以它只应是缓存查找：先下载好，再让脚本 `import()`。

```kotlin
val cache = mutableMapOf<String, ByteArray>() // 宿主在脚本 import 之前填好
JsEngine(JsEngineConfig(moduleLoader = { name -> cache[name]?.let { JsModuleSource.Bytecode(it) } })).use { engine ->
    engine.evaluate("import('pages/detail').then(m => m.title)") // loader 给出后得到 JsValue.Str(...)
}
```

### 预编译字节码

`JsBytecode.compile` 不需要引擎就能把脚本或模块编成字节码。`runBytecode` 对脚本可以反复执行；用真实名字编译的模块只执行一次并像 `evaluateModule` 一样占用该名字，也可以改为按该名字注册、供其他模块 import。字节码跨架构通用，但绑定到编出它的 SDK 所内嵌的引擎版本（`QuickJs.upstreamCommit`）：文件头只用来识别引擎版本，不匹配时以明确的 `JsException` 拒绝。这个头不是完整性或来源校验，其余内容也不做校验，只加载你自己编出来并保管的字节码。

```kotlin
val page = JsBytecode.compile(pageSource, "pages/list", module = true, strip = JsBytecode.Strip.SOURCE)  // 构建期，或设备上编一次缓存
JsEngine().use { engine ->
    engine.registerModule(JsBytecode.compile(coreSource, "@tiny-ui/core", module = true)) // 返回 "@tiny-ui/core"
    (engine.runBytecode(page) as JsRef).use { ns -> ns.get("default", ObjectTransport.REF) }
    engine.runBytecode(JsBytecode.compile("1 + 1"))                                       // JsValue.Num(2.0)
}
```

`Strip.SOURCE` 去掉源码文本、栈里保留行号；`Strip.DEBUG` 去掉全部调试信息。

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

以 `ObjectTransport.REF` 注册的宿主函数收到的 ref 只在本次调用内有效，`retain()` 可以留住一个。`engine.stats()` 报告还有多少 ref 没关（SDK 自己的测试就是靠它证明没有泄漏），以及引擎自己的内存账目：已用字节、配置的限额、对象 / 字符串 / atom / 函数计数。

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
- `./gradlew :library:nativeShimTest` 在 AddressSanitizer 下跑 C 层的 shim 测试；`:library:buildHostTools` 编出命令行编译器 `qjsc-kmp`（`build/native/host-tools/bin`），供构建链使用。
- CI（`.github/workflows/build.yml`）在每个 PR 与推到 `main` 时跑 shim 测试、macOS 测试、Android 宿主测试、iOS 编译、Android AAR 打包与 API 检查；`publish.yml` 在推版本 tag 时发布到 Maven Central。

## 上游

引擎以 `git subtree` 引入到 `native/quickjs`，锁定在 `native/UPSTREAM` 记录的 commit，运行时通过 `QuickJs.upstreamCommit` 可查。只编译引擎核心（`quickjs.c`、`libregexp.c`、`libunicode.c`、`cutils.c`、`dtoa.c`），不链接 `quickjs-libc`，所以 `console.log`、`print` 与 `performance.now` 由 shim 提供，其余（定时器、模块加载）由宿主提供。

## 许可证

MIT。QuickJS 本身为 MIT，版权归 Fabrice Bellard 与 Charlie Gordon。

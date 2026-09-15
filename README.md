# quickjs-kmp

> Kotlin Multiplatform bindings for [QuickJS](https://github.com/bellard/quickjs), the ES2025 JavaScript engine by Fabrice Bellard. Android + iOS, with macOS as a debug host.

[![Platform](https://img.shields.io/badge/Platform-Android%20%7C%20iOS-brightgreen)](https://kotlinlang.org/docs/multiplatform.html)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

English | [中文](./README_ZH.md)

> Not published yet. Successor of [mquickjs-kmp](https://github.com/HarlonWang/mquickjs-kmp) (archived); see [docs/roadmap.md](docs/roadmap.md) for what is done and what is next. Built for [TinyUI](https://github.com/HarlonWang/tinyui) but not tied to it.

## Platforms

| Target | Binding | Notes |
| --- | --- | --- |
| Android (`minSdk 24`) | JNI | `.so` bundled in the AAR (`arm64-v8a`, `armeabi-v7a`, `x86_64`) |
| iOS (`iosArm64`, `iosSimulatorArm64`) | cinterop | static library bundled in the klib, no CocoaPods / SPM |
| macOS (`macosArm64`) | cinterop | debug host for ASan, published as well |

## Usage

```kotlin
JsEngine(JsEngineConfig(memoryLimit = 8L * 1024 * 1024, logger = ::println)).use { engine ->
    engine.registerFunction("discount") { args ->
        val amount = (args[0] as JsValue.Num).value
        JsValue.Num(if (amount > 100) amount * 0.9 else amount)
    }
    engine.evaluate("const total = discount(120);")
    engine.evaluate("total")                      // JsValue.Num(108.0)
    engine.evaluate("({ok: total > 100})")        // JsValue.Json("{\"ok\":true}")
    engine.evaluate("console.log('done', total)") // logger receives "done 108"
}
```

- Primitives cross the boundary as `JsValue.Num` / `Str` / `Bool` / `Null` / `Undefined`, `BigInt` as decimal text in `JsValue.BigInt`, `ArrayBuffer` and typed arrays as a copy of their bytes in `JsValue.Bytes` (a `Bytes` handed to JS becomes an `ArrayBuffer`); other objects and arrays as `JsValue.Json` by default. JSON drops what `JSON.stringify` drops (functions, `undefined` properties, `Map` / `Set` contents); ask for `ObjectTransport.REF` when that matters.
- A script that throws, fails to parse, or hits `memoryLimit` raises `JsException` with the engine's message and `stack`.
- Throwing from a host function surfaces in JS as an `Error` with the Kotlin message.
- `engine.interrupt()` may be called from any thread and stops the running script with an uncatchable `InternalError: interrupted`.
- `JsEngineConfig.maxStackSize` (default 256 KB) must stay below the stack of the thread that runs the engine.
- The engine is single-threaded; use `JsRuntime` (below) or serialize access yourself.

### Promises and `async`

Every outermost engine call drains the microtask queue before it returns, so `then` callbacks and `await` continuations run inside the same call. A Promise result is unwrapped: fulfilled gives its value, rejected throws `JsException`, still pending comes back as a `JsRef` whose `isPromise` is true; the Promise itself may settle during a later call, the flag does not change.

```kotlin
JsEngine(JsEngineConfig(onUnhandledRejection = { e -> println("lost: ${e.message}") })).use { engine ->
    engine.evaluate("async function total(a, b) { await null; return a + b; }")
    engine.evaluate("total(1, 2)")                       // JsValue.Num(3.0)
    engine.evaluate("Promise.reject(new Error('x'))")    // throws JsException("Error: x")
    engine.evaluate("Promise.reject(new Error('y')); 0") // returns 0, handler receives "Error: y"
}
```

Rejections nobody handled by the time the call returns go to `onUnhandledRejection`, or to `logger` as one line when no handler is set. Interrupting a call also discards the microtasks it left behind.

### ES modules

Modules are looked up in a name table by the exact specifier scripts use: register the sources the page may import, then evaluate the page module and read its namespace. There is no file-system loader and no relative-path resolution, so a bundler must flatten the module graph to bare names.

```kotlin
JsEngine(JsEngineConfig(moduleScheme = "app")).use { engine ->
    engine.registerModule("util", "export const url = import.meta.url; export function twice(n) { return n * 2; }")
    engine.evaluateModule("import { twice, url } from 'util'; export default twice(21); export const from = url;", name = "main").use { ns ->
        ns.get("default") // JsValue.Num(42.0)
        ns.get("from")    // JsValue.Str("app:util")
    }
}
```

`evaluateModule` returns the namespace as a `JsRef`, and the module can be imported by its name afterwards. A registered module is compiled at its first import and runs once; registered and evaluated names share one namespace, so claiming a name twice throws (names in angle brackets, like the default, are anonymous). Top-level `await` is supported: a module still pending when the call returns comes back as a `JsRef` with `isPromise`. Each `evaluateModule` call leaves the compiled module in the engine for its lifetime.

### Holding JS objects: `JsRef`

Ask for `ObjectTransport.REF` and objects come back as live handles instead of JSON. A `JsRef` reads and writes properties, indexes arrays, calls functions with a `this` and arguments, and must be closed: the object stays alive in the engine until then.

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

Host functions registered with `ObjectTransport.REF` receive refs that live only for the duration of the call; `retain()` keeps one. `engine.stats().liveRefs` tells you how many refs are still open, which is how the SDK's own tests prove nothing leaks.

### Typed values: kotlinx.serialization

`@Serializable` types cross the boundary with kotlinx.serialization (the runtime ships with the SDK; add the compiler plugin to your own module): primitives become `JsValue.Num` / `Str` / `Bool` / `Null`, everything else becomes JSON text. `Json.encodeToJsValue` / `Json.decodeFromJsValue` are the building blocks; `JsValue.decode<T>()`, `JsEngine.evaluateAs<T>()` and the typed `registerFunction` overloads (one to three arguments) are shortcuts.

### Coroutines: `JsRuntime`

`JsRuntime` serializes every access to one engine under a mutex, runs the work on a dispatcher of your choice (default: a single lane of `Dispatchers.Default`), and maps cancellation and timeouts to engine interrupts.

```kotlin
val runtime = JsRuntime()
try {
    try {
        runtime.evaluate("for (;;) {}", timeout = 200.milliseconds)
    } catch (e: TimeoutCancellationException) {
        // the script was interrupted; the engine stays usable
    }
    runtime.withEngine { evaluate("1 + 1") } // exclusive access, refs usable inside
} finally {
    runtime.shutdown()
}
```

## Documentation

- [docs/architecture.md](docs/architecture.md): layering, the handle table, the host-function trampoline, threading
- [docs/native-build.md](docs/native-build.md): upstream vendoring and per-target builds
- [docs/decisions.md](docs/decisions.md): why things are named and scoped the way they are
- [docs/roadmap.md](docs/roadmap.md): milestones

## Building

- JDK 25 for the Gradle daemon (`gradle/gradle-daemon-jvm.properties`; Gradle downloads it when missing), Xcode, Android SDK with the NDK version pinned in `gradle/libs.versions.toml`, and `cmake` on `PATH`.
- `./gradlew :library:macosArm64Test` is the fastest full check; `:library:testAndroidHostTest` runs the same suite through the real JNI bridge on the host; `:library:connectedAndroidDeviceTest` runs it on a device or emulator.
- `./gradlew :library:nativeShimTest` runs the C-level shim tests under AddressSanitizer.
- CI (`.github/workflows/build.yml`) runs the shim tests, macOS tests, Android host tests, iOS compilation, Android AAR assembly and the API check on every PR and push to `main`; `publish.yml` releases to Maven Central when a version tag is pushed.

## Upstream

The engine is vendored under `native/quickjs` with `git subtree`, pinned to the commit recorded in `native/UPSTREAM`. `QuickJs.upstreamCommit` exposes that commit at runtime. Only the engine core is compiled (`quickjs.c`, `libregexp.c`, `libunicode.c`, `cutils.c`, `dtoa.c`); `quickjs-libc` is not linked, so `console.log`, `print` and `performance.now` come from the shim and everything else (timers, module loading) comes from the host.

## License

MIT. QuickJS itself is MIT, copyright Fabrice Bellard and Charlie Gordon.

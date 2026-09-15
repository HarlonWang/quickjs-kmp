# 原生构建

## 目录

```
native/
├── UPSTREAM     上游 commit 锁定（唯一真值，Gradle 读取后注入 BuildInfo，CMake 读取后注入 KMPJS_UPSTREAM_COMMIT）
├── quickjs/     bellard/quickjs，git subtree，禁止直接修改（不叫 upstream：macOS 不区分大小写，会与 UPSTREAM 文件撞名）
├── patches/     对上游的补丁，构建时 apply
├── shim/        quickjs_kmp.c/.h
├── jni/         JNI 胶水，只服务 Android
└── test/        shim 的 C 测试
```

## 上游同步

上游没有 tag，只能锁 commit。拉取与更新都走 subtree：

```sh
# 首次
git subtree add --prefix native/quickjs https://github.com/bellard/quickjs.git <commit> --squash
# 更新
git subtree pull --prefix native/quickjs https://github.com/bellard/quickjs.git <commit> --squash
```

更新后同步改 `native/UPSTREAM` 的 `commit=` 与 `date=`。需要改上游代码时一律写进 `patches/`，保证 `subtree pull` 永远能干净合入。

## 编译范围

只编引擎核心五个文件：`quickjs.c`、`libregexp.c`、`libunicode.c`、`cutils.c`、`dtoa.c`，加上 `shim/quickjs_kmp.c`。不编 `quickjs-libc.c`（`std` / `os` 模块、文件 IO）、`qjs.c`（REPL）、`qjsc.c`。`CONFIG_VERSION` 由 CMake 从上游 `VERSION` 文件读入。

## 各目标

| 目标 | 编译 | 绑定 | 产物 |
|---|---|---|---|
| Android | CMake + NDK 工具链，`arm64-v8a` / `armeabi-v7a` / `x86_64` | JNI | AAR 内含 `.so` |
| iosArm64 / iosSimulatorArm64 | Xcode 工具链编出 `.a` | cinterop `.def`，`staticLibraries` 打进 klib | 使用方无需 CocoaPods / SPM |
| macosArm64 | 同上 | 同上 | 调试宿主，随包发布 |

三端都由 `native/CMakeLists.txt` 统一描述，Gradle 的 `CMakeBuild` 任务按目标传不同的 CMake 参数。AGP 9 的 KMP 库插件没有 `externalNativeBuild` DSL，Android 的 `.so` 由 `collectJniLibs` 汇总后经变体 API `sources.jniLibs.addGeneratedSourceDirectory` 注入 AAR。Apple 侧 cinterop 的 `.def` 用 `staticLibraries` 把 `.a` 打进 klib，`-libraryPath` 按目标传入。

NDK 版本固定在 version catalog 的 `android-ndk`，不用 AGP 默认值，避免 CI 与本机各自下载不同版本。`cmake` 取 PATH 上的（brew 或 Android SDK 自带的均可）。

Android host test 走宿主编译的 JNI 库：`buildNativeHostJni` 以 `-DQJS_HOST_JNI=ON` 在本机编出 `libquickjs_kmp.dylib`（Linux 为 `.so`），`testAndroidHostTest` 通过 `java.library.path` 加载。JDK 头文件按 `JAVA_HOME` → daemon 的 `java.home` → `/Library/Java/JavaVirtualMachines/*` 顺序找第一个带 `include/jni.h` 的（Android Studio 的 JBR 没有头文件）。设备测试仍走 `connectedAndroidDeviceTest`。

## 调试宿主：ASan 的 shim 测试

`native/test/shim_test.c` 是直接对 C API 的断言测试，由 `buildNativeShimTest` 以 `-DQJS_SHIM_TEST=ON -DQJS_ASAN=ON -DCMAKE_BUILD_TYPE=Debug` 在宿主上编译，`nativeShimTest` 运行并挂在 `check` 下、CI 门禁里。ASan 抓越界与悬垂；Debug 构建保留 `assert`，`JS_FreeRuntime` 的「堆已空」断言因此在测试里生效，任何 `JSValue` 泄漏都会在销毁引擎时暴露。Kotlin 层不持有 `JSValue`，所以 ASan 只需覆盖 C 层，不为它单独编 K/N 测试库。

## 消费方本地联调

消费方 App 若遵循 `local.properties` 的 composite build 约定，在其 `local.properties` 写 `quickjs-kmp.dir=<本仓路径>` 即可直接从源码构建本 SDK；坐标到项目路径的映射由本仓 `gradle/composite-substitutions` 声明。消费方的 CI 没有 `local.properties`，仍解析 Maven 版本，发版后记得 bump。

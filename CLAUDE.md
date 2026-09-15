# quickjs-kmp

QuickJS 的 KMP 绑定 SDK，公共 API 与 mquickjs-kmp 同形。开始工作前先读 README.md，再按需读 docs/。

## 结构

- `library/`：核心模块，artifactId `quickjs-kmp`（主模块叫 library，扩展模块用自己的名字并以 `quickjs-kmp-` 为产物前缀），包名 `wang.harlon.quickjs`；kotlinx.serialization 类型化桥接在 `*Serialization.kt`
- `native/quickjs/`：上游 git subtree，**禁止直接修改**，改动进 `native/patches/`
- `native/shim/`：唯一的 C API 层，JNI 与 cinterop 都只对接它
- `native/UPSTREAM`：上游 commit 唯一真值，Gradle 读它生成 `BuildInfo.kt`，CMake 读它注入 `KMPJS_UPSTREAM_COMMIT`
- `docs/decisions.md`：为什么这么定；改决策时更新条目，不追加叙事

## 约定

- 依赖坐标只在 `gradle/libs.versions.toml` 声明
- `README.md` 与 `README_ZH.md` 必须在同一个 commit 里改完
- 公共 API 改动要跑 `./gradlew apiDump`，各模块 `api/` 目录入库
- 平台绑定只对接 `native/shim`，不直接对接 `quickjs.h`；Kotlin 侧永远不持有原生 `JSValue`（原因见 docs/architecture.md）
- shim 里每个持有 `JSValue` 的地方都要在引擎 close 前释放：`JS_FreeRuntime` 断言堆已空，三端构建不定义 `NDEBUG`

## 构建与测试

- Gradle daemon 用 JDK 25（`gradle/gradle-daemon-jvm.properties`，缺失自动下载）、Xcode、Android SDK（NDK 版本见 version catalog）、PATH 上有 `cmake`、`local.properties` 里 `sdk.dir`
- `gradle/composite-substitutions` 是消费方 composite build 的契约（坐标 → 项目路径），改模块名必须同步
- CI：`.github/workflows/build.yml`（PR 与 main：shim C 测试、macOS 测试、Android host 测试、iOS 编译、Android AAR、apiCheck），`publish.yml`（推 `x.y.z` tag 触发正式发布，版本号从 tag 注入）；不发快照，本地联调靠 `gradle/composite-substitutions`；secrets 命名与 kmp-webview 相同
- 原生链路：`buildNative*`（CMake）→ Android `collectJniLibs` / Apple cinterop，全部由 Gradle 驱动，详见 docs/native-build.md
- macOS 单测（最快的反馈）：`./gradlew :library:macosArm64Test`
- Android host test（真实 JNI 路径，不需要模拟器）：`./gradlew :library:testAndroidHostTest`
- shim 的 C 测试（ASan）：`./gradlew :library:nativeShimTest`，改 shim 必跑
- Android 设备测试（需模拟器在线）：`./gradlew :library:connectedAndroidDeviceTest`
- iOS 只编译：`./gradlew :library:compileKotlinIosArm64 :library:compileKotlinIosSimulatorArm64`
- 改 shim 时新增的行为要在 `native/test/shim_test.c` 里加 CHECK

package wang.harlon.quickjs

/**
 * Entry point metadata for the QuickJS Kotlin Multiplatform bindings.
 */
object QuickJs {
    /** Version of this SDK, as published to Maven Central. */
    val sdkVersion: String = BuildInfo.SDK_VERSION

    /** Full git commit of the vendored bellard/quickjs engine this SDK was built from. */
    val upstreamCommit: String = BuildInfo.UPSTREAM_COMMIT
}

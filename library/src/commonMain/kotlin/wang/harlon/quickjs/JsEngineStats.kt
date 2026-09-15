package wang.harlon.quickjs

/**
 * Diagnostics snapshot. [liveRefs] is the number of [JsRef.close] calls still owed: every open
 * [JsRef] counts one, a [JsRef.retain] adds one more, and transient host-function arguments count
 * during the call; [refSlots] is how many handle slots the engine has allocated so far (live plus reusable).
 * The remaining fields come from the engine's own accounting (`JS_ComputeMemoryUsage`).
 * @property memoryUsed bytes currently allocated by the engine
 * @property memoryLimit the configured [JsEngineConfig.memoryLimit], 0 when unlimited
 */
class JsEngineStats(
    val liveRefs: Int,
    val refSlots: Int,
    val memoryUsed: Long,
    val memoryLimit: Long,
    val objectCount: Long,
    val stringCount: Long,
    val atomCount: Long,
    val functionCount: Long,
) {
    override fun toString(): String =
        "JsEngineStats(liveRefs=$liveRefs, refSlots=$refSlots, memoryUsed=$memoryUsed, memoryLimit=$memoryLimit, " +
            "objectCount=$objectCount, stringCount=$stringCount, atomCount=$atomCount, functionCount=$functionCount)"
}

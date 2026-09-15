package wang.harlon.quickjs

internal interface HostCallbacks {
    fun onHostCall(id: Int, args: List<RawValue>): RawValue
    fun onLog(message: String)
    fun onUnhandledRejection(reason: RawValue)
}

/** Mirror of `kmpjs_value` (native/shim/quickjs_kmp.h); the only shape that crosses the native boundary. */
internal class RawValue(
    val tag: Int,
    val ref: Long = 0L,
    val num: Double = 0.0,
    val str: String? = null,
    val stack: String? = null,
    /** KMPJS_TAG_BINARY payload; [str] is null then. */
    val bytes: ByteArray? = null,
)

/** Tags and flags mirror KMPJS_TAG_* / KMPJS_FLAG_* / KMPJS_REF_* in native/shim/quickjs_kmp.h. */
internal object NativeTag {
    const val UNDEFINED = 0
    const val NULL = 1
    const val BOOL = 2
    const val NUMBER = 3
    const val STRING = 4
    const val OBJECT = 5
    const val EXCEPTION = 6
    const val REF = 7
    const val BIGINT = 8
    const val BINARY = 9

    const val FLAG_REF_OBJECTS = 1
    const val REF_FUNCTION = 1
    const val REF_ARRAY = 2
    const val REF_PROMISE = 4
}

internal expect class NativeEngine(config: JsEngineConfig, host: HostCallbacks) {
    fun evaluate(script: String, fileName: String, flags: Int): RawValue
    fun defineFunction(name: String, id: Int, flags: Int): RawValue
    fun interrupt()
    fun close()

    fun refRetain(ref: Long)
    fun refRelease(ref: Long)
    fun refGet(ref: Long, name: String, flags: Int): RawValue
    fun refGetIndex(ref: Long, index: Int, flags: Int): RawValue
    fun refSet(ref: Long, name: String, value: RawValue): RawValue
    fun refCall(ref: Long, thisRef: Long, args: List<RawValue>, flags: Int): RawValue
    fun refToJson(ref: Long): RawValue

    /** [liveRefs, refSlots] as in kmpjs_stats. */
    fun stats(): IntArray
    fun dumpMemory(): RawValue
}

internal fun Throwable.hostErrorMessage(): String = message ?: this::class.simpleName ?: "host error"

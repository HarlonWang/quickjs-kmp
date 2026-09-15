package wang.harlon.quickjs

internal object NativeBridge {
    init {
        System.loadLibrary("quickjs_kmp")
        val abi = nativeAbiVersion()
        check(abi == ABI_VERSION) { "libquickjs_kmp ABI $abi does not match Kotlin side $ABI_VERSION" }
    }

    const val ABI_VERSION = 4

    @JvmStatic external fun nativeAbiVersion(): Int
    @JvmStatic external fun nativeCreate(memoryLimit: Long, maxStackSize: Long, gcThreshold: Long, moduleScheme: ByteArray, target: Any): Long
    @JvmStatic external fun nativeDestroy(ptr: Long)
    @JvmStatic external fun nativeEval(ptr: Long, code: ByteArray, fileName: ByteArray, flags: Int): NativeValue?
    @JvmStatic external fun nativeDefineFunction(ptr: Long, name: ByteArray, id: Int, flags: Int): NativeValue?
    @JvmStatic external fun nativeInterrupt(ptr: Long)
    @JvmStatic external fun nativeRefRetain(ptr: Long, ref: Long)
    @JvmStatic external fun nativeRefRelease(ptr: Long, ref: Long)
    @JvmStatic external fun nativeRefGet(ptr: Long, ref: Long, name: ByteArray, flags: Int): NativeValue?
    @JvmStatic external fun nativeRefGetIndex(ptr: Long, ref: Long, index: Int, flags: Int): NativeValue?
    @JvmStatic external fun nativeRefSet(ptr: Long, ref: Long, name: ByteArray, value: NativeValue): NativeValue?
    @JvmStatic external fun nativeRefCall(ptr: Long, ref: Long, thisRef: Long, args: Array<NativeValue>, flags: Int): NativeValue?
    @JvmStatic external fun nativeRefToJson(ptr: Long, ref: Long): NativeValue?
    @JvmStatic external fun nativeRegisterModule(ptr: Long, name: ByteArray, code: ByteArray): NativeValue?
    @JvmStatic external fun nativeEvalModule(ptr: Long, code: ByteArray, name: ByteArray, flags: Int): NativeValue?
    @JvmStatic external fun nativeStats(ptr: Long): IntArray?
    @JvmStatic external fun nativeDumpMemory(ptr: Long): NativeValue?

    @JvmStatic
    fun onHostCall(target: Any, id: Int, args: Array<NativeValue?>): NativeValue {
        val engine = target as NativeEngine
        return try {
            engine.host.onHostCall(id, args.map { it?.toRaw() ?: RawValue(NativeTag.UNDEFINED) }).toNative()
        } catch (t: Throwable) {
            t.toHostError().toNative()
        }
    }

    @JvmStatic
    fun onLog(target: Any, message: ByteArray) {
        (target as NativeEngine).host.onLog(Wtf8.decode(message))
    }

    @JvmStatic
    fun onUnhandledRejection(target: Any, reason: NativeValue) {
        (target as NativeEngine).host.onUnhandledRejection(reason.toRaw())
    }
}

internal fun NativeValue.toRaw(): RawValue =
    if (tag == NativeTag.BINARY) RawValue(tag, bytes = str ?: ByteArray(0))
    else RawValue(tag, ref, num, str?.let(Wtf8::decode), stack?.let(Wtf8::decode))

internal fun RawValue.toNative(): NativeValue =
    NativeValue(tag, ref, num, bytes ?: str?.let(Wtf8::encode), stack?.let(Wtf8::encode))

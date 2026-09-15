package wang.harlon.quickjs

/** Constructed and read by native/jni/quickjs_jni.c; field names and the constructor signature are ABI. */
internal class NativeValue(
    @JvmField val tag: Int,
    @JvmField val ref: Long,
    @JvmField val num: Double,
    @JvmField val str: ByteArray?,
    @JvmField val stack: ByteArray?,
)

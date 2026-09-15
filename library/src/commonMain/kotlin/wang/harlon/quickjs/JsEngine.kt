package wang.harlon.quickjs

import kotlin.concurrent.atomics.AtomicBoolean
import kotlin.concurrent.atomics.AtomicInt
import kotlin.concurrent.atomics.ExperimentalAtomicApi
import kotlin.concurrent.atomics.decrementAndFetch
import kotlin.concurrent.atomics.incrementAndFetch

/**
 * One QuickJS context. Not thread-safe: use it from a single thread, or serialize access
 * (see [JsRuntime]). [interrupt] is the only member safe to call from another thread.
 */
@OptIn(ExperimentalAtomicApi::class)
class JsEngine(private val config: JsEngineConfig = JsEngineConfig()) : AutoCloseable {
    private val functions = ArrayList<JsHostFunction>()
    private val closed = AtomicBoolean(false)
    private val inFlightInterrupts = AtomicInt(0)

    private val callbacks = object : HostCallbacks {
        // 传给宿主函数的 ref 只在本次调用内有效：返回后失效，原生侧随即释放；retain() 的副本不受影响
        override fun onHostCall(id: Int, args: List<RawValue>): RawValue {
            val decoded = args.map { decode(it) }
            decoded.forEach { (it as? JsRef)?.transient = true }
            try {
                return encode(functions[id].invoke(decoded))
            } finally {
                decoded.forEach { (it as? JsRef)?.close() }
            }
        }

        // logger 异常不能穿回原生回调（Kotlin/Native 会直接终止进程），三端统一吞掉
        override fun onLog(message: String) {
            try {
                config.logger?.invoke(message)
            } catch (_: Throwable) {
            }
        }

        override fun onUnhandledRejection(reason: RawValue) {
            val message = reason.str ?: "unknown exception"
            try {
                val handler = config.onUnhandledRejection
                if (handler != null) handler(JsException(message, reason.stack)) else config.logger?.invoke("Unhandled promise rejection: $message")
            } catch (_: Throwable) {
            }
        }
    }

    internal val native = NativeEngine(config, callbacks)

    /**
     * Compiles and runs [script], returning the value of its last expression statement. Microtasks run
     * before this returns; a Promise result is unwrapped (fulfilled: value, rejected: [JsException], pending: [JsRef]).
     * @throws JsException when the script throws, fails to parse, or exhausts memory.
     */
    fun evaluate(
        script: String,
        fileName: String = "<eval>",
        objects: ObjectTransport = ObjectTransport.JSON,
    ): JsValue {
        checkOpen()
        return decode(native.evaluate(script, fileName, objects.flags))
    }

    /**
     * Exposes [function] to scripts as the global [name]. With [ObjectTransport.REF] the function
     * receives object arguments as [JsRef]s that live only for the duration of the call.
     */
    fun registerFunction(
        name: String,
        objects: ObjectTransport = ObjectTransport.JSON,
        function: JsHostFunction,
    ) {
        checkOpen()
        require(isIdentifier(name)) { "'$name' is not a valid JavaScript identifier" }
        functions.add(function)
        try {
            decode(native.defineFunction(name, functions.size - 1, objects.flags))
        } catch (e: JsException) {
            functions.removeAt(functions.size - 1)
            throw e
        }
    }

    /**
     * Makes [source] importable as the ES module [name], exactly as scripts spell the specifier:
     * there is no relative-path resolution. The module is compiled at its first import.
     * @throws JsException when [name] is already registered
     */
    fun registerModule(name: String, source: String) {
        checkOpen()
        decode(native.registerModule(name, source))
    }

    /**
     * Compiles and runs [source] as an ES module and returns its namespace object, from which
     * `default` and named exports can be read. A module still awaiting at top level when this
     * returns comes back as a [JsRef] with [JsRef.isPromise] instead. Every call leaves the compiled
     * module in the engine for its whole lifetime.
     * @throws JsException when the module or one it imports fails to compile, resolve or run
     */
    fun evaluateModule(source: String, name: String = "<module>"): JsRef {
        checkOpen()
        return decode(native.evalModule(source, name, NativeTag.FLAG_REF_OBJECTS)) as JsRef
    }

    /**
     * Asks running script code to stop; the pending evaluation or call then throws [JsException].
     * Safe to call from any thread, including concurrently with [close].
     */
    fun interrupt() {
        inFlightInterrupts.incrementAndFetch()
        try {
            if (!closed.load()) native.interrupt()
        } finally {
            inFlightInterrupts.decrementAndFetch()
        }
    }

    /** Diagnostics: how many refs are alive. Useful in tests to prove nothing leaked. */
    fun stats(): JsEngineStats {
        checkOpen()
        val raw = native.stats()
        return JsEngineStats(liveRefs = raw[0], refSlots = raw[1])
    }

    /** The engine's own heap summary (`JS_DumpMemory`), one line per block type. Diagnostics only. */
    fun dumpMemory(): String {
        checkOpen()
        return (decode(native.dumpMemory()) as JsValue.Str).value
    }

    /** Releases every [JsRef] as well: the engine's whole memory goes away with it. */
    @Suppress("ControlFlowWithEmptyBody")
    override fun close() {
        if (!closed.compareAndSet(expectedValue = false, newValue = true)) return
        // 有意自旋：interrupt() 可能刚通过 closed 检查、还没调到原生层，窗口只有几条指令，等它走完再释放句柄
        while (inFlightInterrupts.load() != 0) {
        }
        native.close()
    }

    internal fun refOp(block: JsEngine.() -> RawValue?): JsValue {
        checkOpen()
        return block()?.let { decode(it) } ?: JsValue.Undefined
    }

    internal val isOpen: Boolean
        get() = !closed.load()

    internal fun releaseRef(id: Long) {
        if (!closed.load()) native.refRelease(id)
    }

    internal fun checkOwned(ref: JsRef) {
        require(ref.engine === this) { "JsRef belongs to another engine" }
    }

    internal fun decode(raw: RawValue): JsValue = when (raw.tag) {
        NativeTag.UNDEFINED -> JsValue.Undefined
        NativeTag.NULL -> JsValue.Null
        NativeTag.BOOL -> JsValue.Bool(raw.num != 0.0)
        NativeTag.NUMBER -> JsValue.Num(raw.num)
        NativeTag.STRING -> JsValue.Str(raw.str ?: "")
        NativeTag.OBJECT -> JsValue.Json(raw.str)
        NativeTag.REF -> JsRef(this, raw.ref, raw.num.toInt())
        NativeTag.BIGINT -> JsValue.BigInt(raw.str ?: "0")
        NativeTag.BINARY -> JsValue.Bytes(raw.bytes ?: ByteArray(0))
        NativeTag.EXCEPTION -> throw JsException(raw.str ?: "unknown exception", raw.stack)
        else -> error("unknown native tag ${raw.tag}")
    }

    internal fun encode(value: JsValue): RawValue = when (value) {
        JsValue.Undefined -> RawValue(NativeTag.UNDEFINED)
        JsValue.Null -> RawValue(NativeTag.NULL)
        is JsValue.Bool -> RawValue(NativeTag.BOOL, num = if (value.value) 1.0 else 0.0)
        is JsValue.Num -> RawValue(NativeTag.NUMBER, num = value.value)
        is JsValue.Str -> RawValue(NativeTag.STRING, str = value.value)
        is JsValue.Json -> RawValue(NativeTag.OBJECT, str = value.json)
        is JsValue.BigInt -> RawValue(NativeTag.BIGINT, str = value.value)
        is JsValue.Bytes -> RawValue(NativeTag.BINARY, bytes = value.value)
        is JsRef -> {
            checkOwned(value)
            check(value.isValid) { "JsRef is closed" }
            RawValue(NativeTag.REF, ref = value.id)
        }
    }

    private fun checkOpen() {
        check(!closed.load()) { "JsEngine is closed" }
    }

    private fun isIdentifier(name: String): Boolean =
        name.isNotEmpty() &&
            (name[0].isLetter() || name[0] == '_' || name[0] == '$') &&
            name.all { it.isLetterOrDigit() || it == '_' || it == '$' }
}

internal fun Throwable.toHostError(): RawValue = RawValue(NativeTag.EXCEPTION, str = hostErrorMessage())

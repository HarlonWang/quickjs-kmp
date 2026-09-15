package wang.harlon.quickjs

/**
 * Ahead-of-time compilation. The output is bound to the engine embedded in this SDK version
 * (see [QuickJs.upstreamCommit]) and is otherwise portable across architectures; the engine
 * rejects bytecode from another engine build with a [JsException]. Nothing else about the bytes
 * is validated, so only load what this SDK produced.
 */
object JsBytecode {
    /** How much debug information the bytecode keeps. */
    enum class Strip {
        /** Everything, including the source text. */
        NONE,

        /** Drops the source text; stack traces keep line numbers. */
        SOURCE,

        /** Drops all debug information; stack traces have no locations. */
        DEBUG,
    }

    /**
     * Compiles [source] as a script, or as an ES module when [module] is true. A module is compiled
     * under the name [fileName], which is what it must be registered as ([JsEngine.registerModule]).
     * @throws JsException on syntax errors, with the location in [JsException.jsStack]
     */
    fun compile(
        source: String,
        fileName: String = "<bytecode>",
        module: Boolean = false,
        strip: Strip = Strip.NONE,
    ): ByteArray {
        val flags = (if (module) NativeTag.COMPILE_MODULE else 0) or when (strip) {
            Strip.NONE -> 0
            Strip.SOURCE -> NativeTag.COMPILE_STRIP_SOURCE
            Strip.DEBUG -> NativeTag.COMPILE_STRIP_DEBUG
        }
        return when (val result = NativeCompiler.compile(source, fileName, flags)) {
            is ByteArray -> result
            is RawValue -> throw JsException(result.str ?: "compilation failed", result.stack)
            else -> error("unexpected compiler result $result")
        }
    }
}

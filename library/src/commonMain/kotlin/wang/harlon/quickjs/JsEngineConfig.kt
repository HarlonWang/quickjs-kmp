package wang.harlon.quickjs

/**
 * @property memoryLimit upper bound in bytes for the engine heap; 0 means unlimited. Running out
 * raises a [JsException].
 * @property maxStackSize bytes of native stack the engine may use before throwing `RangeError`.
 * Must stay below the stack of the thread that runs the engine; 0 disables the check.
 * @property gcThreshold bytes allocated between garbage collection cycles; 0 keeps the engine default.
 * @property logger receives each `console.log` / `print` line; exceptions it throws are swallowed.
 * @property onUnhandledRejection receives every promise rejected during an engine call and still
 * unhandled when that call returns; when null the rejection goes to [logger] as one line. Exceptions
 * it throws are swallowed.
 * @property moduleScheme prefix of `import.meta.url`, which reads `<moduleScheme>:<module name>`.
 * @property moduleLoader asked for a module no [JsEngine.registerModule] call has claimed, at its
 * first import (static or dynamic `import()`), on the thread running the engine and inside the
 * importing call. A module it returns is cached under that name and never asked again; null
 * ("unknown", the import throws `ReferenceError`) or an exception (reaching the importer as an
 * `Error`) spends nothing, so the next import of that name asks again. It must not call the
 * engine; download ahead of time and serve from a cache here.
 */
class JsEngineConfig(
    val memoryLimit: Long = 0,
    val maxStackSize: Long = DEFAULT_MAX_STACK_SIZE,
    val gcThreshold: Long = 0,
    val logger: ((String) -> Unit)? = null,
    val onUnhandledRejection: ((JsException) -> Unit)? = null,
    val moduleScheme: String = "kmp",
    val moduleLoader: ((name: String) -> JsModuleSource?)? = null,
) {
    init {
        require(memoryLimit >= 0) { "memoryLimit must not be negative" }
        require(maxStackSize >= 0) { "maxStackSize must not be negative" }
        require(gcThreshold >= 0) { "gcThreshold must not be negative" }
        require(moduleScheme.isNotEmpty() && ':' !in moduleScheme) { "moduleScheme must be a non-empty URL scheme" }
    }

    companion object {
        const val DEFAULT_MAX_STACK_SIZE: Long = 256 * 1024
    }
}

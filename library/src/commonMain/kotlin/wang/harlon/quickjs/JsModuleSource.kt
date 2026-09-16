package wang.harlon.quickjs

/** What [JsEngineConfig.moduleLoader] hands back for a module name. */
sealed class JsModuleSource {
    /** ES module source text. */
    class Text(val code: String) : JsModuleSource()

    /** Output of [JsBytecode.compile] with `module = true`, compiled under exactly the requested name. */
    class Bytecode(val bytes: ByteArray) : JsModuleSource()
}

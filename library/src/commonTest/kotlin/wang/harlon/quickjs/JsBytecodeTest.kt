package wang.harlon.quickjs

import kotlin.io.encoding.Base64
import kotlin.io.encoding.ExperimentalEncodingApi
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

class JsBytecodeTest {
    @Test
    fun compiledScriptsRunAnyNumberOfTimes() = JsEngine().use { engine ->
        engine.registerFunction("report") { it[0] }
        val bytes = JsBytecode.compile("var runs = (typeof runs === 'number' ? runs : 0) + 1; report(runs) * 10 + 2", "prog.js")
        assertTrue(bytes.size > 52)
        assertEquals(JsValue.Num(12), engine.runBytecode(bytes))
        assertEquals(JsValue.Num(22), engine.runBytecode(bytes))
        engine.evaluate("runs = 41")
        assertEquals(JsValue.Num(422), engine.runBytecode(bytes))
        assertEquals(JsValue.Json("""{"ok":true}"""), engine.runBytecode(JsBytecode.compile("({ok: true})")))
        assertIs<JsRef>(engine.runBytecode(JsBytecode.compile("({ok: true})"), objects = ObjectTransport.REF)).close()
    }

    @Test
    fun syntaxErrorsSurfaceAtCompileTime() {
        val e = assertFailsWith<JsException> { JsBytecode.compile("var x = ;", "bad.js") }
        assertTrue(e.message.orEmpty().startsWith("SyntaxError"), "message was: ${e.message}")
        assertTrue(e.jsStack.orEmpty().contains("bad.js"), "stack was: ${e.jsStack}")
    }

    @Test
    fun stripControlsWhatErrorsCanTell() = JsEngine().use { engine ->
        val source = "function boom() { throw new Error('x'); } boom()"
        val full = JsBytecode.compile(source, "loc.js")
        val noSource = JsBytecode.compile(source, "loc.js", strip = JsBytecode.Strip.SOURCE)
        val noDebug = JsBytecode.compile(source, "loc.js", strip = JsBytecode.Strip.DEBUG)
        assertTrue(full.size > noSource.size && noSource.size > noDebug.size, "sizes: ${full.size} ${noSource.size} ${noDebug.size}")
        assertTrue(assertFailsWith<JsException> { engine.runBytecode(full) }.jsStack.orEmpty().contains("loc.js:1"))
        assertTrue(assertFailsWith<JsException> { engine.runBytecode(noSource) }.jsStack.orEmpty().contains("loc.js:1"))
        val stripped = assertFailsWith<JsException> { engine.runBytecode(noDebug) }
        assertEquals("Error: x", stripped.message)
        assertTrue(!stripped.jsStack.orEmpty().contains("loc.js:1"), "stack was: ${stripped.jsStack}")
    }

    @Test
    fun modulesCompileWithoutTheirDependenciesAndRegisterUnderTheirName() = JsEngine(JsEngineConfig(moduleScheme = "app")).use { engine ->
        val page = JsBytecode.compile("import { bump } from 'counter'; export const url = import.meta.url; export default bump();", "bc-page", module = true)
        engine.registerModule("counter", "export let n = 0; export function bump() { return ++n; }")
        assertEquals("bc-page", engine.registerModule(page))
        assertTrue(assertFailsWith<JsException> { engine.registerModule(page) }.message.orEmpty().contains("already registered"))
        engine.evaluateModule("import page, { url } from 'bc-page'; export const r = page + ':' + url;").use { ns ->
            assertEquals(JsValue.Str("1:app:bc-page"), ns.get("r"))
        }
        val direct = JsBytecode.compile("import { bump } from 'counter'; export const n = bump();", "bc-direct", module = true)
        assertIs<JsRef>(engine.runBytecode(direct)).use { assertEquals(JsValue.Num(2), it.get("n")) }
        assertTrue(assertFailsWith<JsException> { engine.runBytecode(direct) }.message.orEmpty().contains("already evaluated"))
        val missing = JsBytecode.compile("import 'nowhere';", "bc-missing", module = true)
        assertTrue(assertFailsWith<JsException> { engine.runBytecode(missing) }.message.orEmpty().contains("'nowhere' is not registered"))
        assertTrue(assertFailsWith<JsException> { engine.registerModule(JsBytecode.compile("1", "script")) }.message.orEmpty().contains("not a module"))
        assertTrue(assertFailsWith<JsException> { engine.registerModule(JsBytecode.compile("export const a = 1;", module = true)) }.message.orEmpty().contains("anonymous"))
        assertEquals(0, engine.stats().liveRefs)
    }

    @Test
    fun bytecodeIsBoundToTheEngineBuild() = JsEngine().use { engine ->
        val bytes = JsBytecode.compile("1")
        val tampered = bytes.copyOf().also { "0000000000".encodeToByteArray().copyInto(it, 12) }
        assertTrue(assertFailsWith<JsException> { engine.runBytecode(tampered) }.message.orEmpty().contains("built for engine 0000"))
        assertTrue(assertFailsWith<JsException> { engine.runBytecode("garbage".encodeToByteArray()) }.message.orEmpty().contains("not QuickJS bytecode"))
        assertTrue(assertFailsWith<JsException> { engine.runBytecode(bytes.copyOf().also { it[8] = 1 }) }.message.orEmpty().contains("does not match its header"))
        assertEquals(JsValue.Num(1), engine.runBytecode(bytes))
    }

    // 固定样本：在 macOS arm64 上编译（strip = SOURCE），含 BigInt 与浮点，验证字节码与字长无关；
    // 上游 commit 变更时会以「built for engine …」失败，此时用 native/test 里的 kmpjs_compile 重新生成
    @OptIn(ExperimentalEncodingApi::class)
    @Test
    fun bytecodeCompiledOnAnotherArchitectureLoads() = JsEngine().use { engine ->
        val fixture = Base64.decode(
            "UUpLQjQAAAABAAAAMDRiZTI0NjAwMTU5OWY1OTk1ZmEyZjJkOGM5MWEwZjE5OGQzZjM0YwUGDmZpeHR1cmUGYmlnCHRleHQCeAZtYXACLA3mAwADAADoAwAB6gMAAiwAAAAMIAYBqAEAAAAEAAQBRADoAwAeAOoDAR4AggICFgDSAgAFAAjoAimwAgAAALBAAAAAnbABAAAAm98E8wAAAOC0vQC7/QT2AAAAJgQAPvcAAAA4AwAkAQA+XwAAAAT4AAAAJAEA4QYv5gMWAABILCAOBxcgSBZKNBAbChEBERA0CgAGAAAAAAAABEA=",
        )
        assertEquals("fixture", engine.registerModule(fixture))
        engine.evaluateModule("import big, { text } from 'fixture'; import * as ns from 'fixture'; export const all = text + '|' + ns.big + '|' + ns.default;").use { ns ->
            assertEquals(JsValue.Str("fixture|18446744073709551617|1,2.5,-3,x"), ns.get("all"))
        }
        assertNull(engine.evaluate("undefined") as? JsRef)
    }
}

package wang.harlon.quickjs

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertTrue
import kotlinx.coroutines.test.runTest

class JsModuleTest {
    @Test
    fun importsRegisteredModulesAndReadsTheNamespace() = JsEngine(JsEngineConfig(moduleScheme = "tinyui")).use { engine ->
        engine.registerModule("@tiny-ui/core", "export const url = import.meta.url; export function h(tag) { return '<' + tag + '>'; }")
        engine.evaluateModule("import { h, url } from '@tiny-ui/core'; export default h('div'); export const seen = url;", name = "pages/list").use { page ->
            assertFalse(page.isPromise)
            assertEquals(JsValue.Str("<div>"), page.get("default"))
            assertEquals(JsValue.Str("tinyui:@tiny-ui/core"), page.get("seen"))
        }
    }

    @Test
    fun aModuleRunsOnceAndImportersShareItsState() = JsEngine().use { engine ->
        engine.registerModule("counter", "export let n = 0; export function bump() { return ++n; }")
        engine.evaluateModule("import { bump } from 'counter'; bump();", name = "a").close()
        engine.evaluateModule("import { n, bump } from 'counter'; export const seen = n; bump();", name = "b").use { b ->
            assertEquals(JsValue.Num(1), b.get("seen"))
        }
        engine.evaluateModule("import { n } from 'counter'; export default n;", name = "c").use { c ->
            assertEquals(JsValue.Num(2), c.get("default"))
        }
        assertEquals(JsValue.Num(1), engine.evaluate("1"))
        assertEquals(0, engine.stats().liveRefs)
    }

    @Test
    fun unknownNamesAreReferenceErrorsAndPathsAreNotResolved() = JsEngine().use { engine ->
        val e = assertFailsWith<JsException> { engine.evaluateModule("import x from 'missing';", name = "bad") }
        assertEquals("ReferenceError: module 'missing' is not registered", e.message)
        val rel = assertFailsWith<JsException> { engine.evaluateModule("import './util.js';", name = "rel") }
        assertTrue(rel.message.orEmpty().contains("'./util.js' is not registered"), "message was: ${rel.message}")
    }

    @Test
    fun errorsSurfaceWhenTheImporterRuns() = JsEngine().use { engine ->
        engine.registerModule("broken", "export const = ;")
        val syntax = assertFailsWith<JsException> { engine.evaluateModule("import 'broken';", name = "c") }
        assertTrue(syntax.message.orEmpty().startsWith("SyntaxError"), "message was: ${syntax.message}")
        val body = assertFailsWith<JsException> { engine.evaluateModule("throw new Error('in body');", name = "d") }
        assertEquals("Error: in body", body.message)
        assertTrue(body.jsStack.orEmpty().contains("d"), "stack was: ${body.jsStack}")
    }

    @Test
    fun duplicateRegistrationIsRejected() = JsEngine().use { engine ->
        engine.registerModule("m", "export const v = 1;")
        val e = assertFailsWith<JsException> { engine.registerModule("m", "export const v = 2;") }
        assertTrue(e.message.orEmpty().contains("already registered"), "message was: ${e.message}")
        engine.evaluateModule("import { v } from 'm'; export default v;", name = "x").use { assertEquals(JsValue.Num(1), it.get("default")) }
    }

    @Test
    fun evaluatedModulesAreImportableAndNamesNeverCollide() = JsEngine().use { engine ->
        engine.evaluateModule("export const page = 'p';", name = "page").close()
        engine.evaluateModule("import { page } from 'page'; export default page + '!';").use { assertEquals(JsValue.Str("p!"), it.get("default")) }
        engine.evaluateModule("export const again = 1;").close()
        val shadow = assertFailsWith<JsException> { engine.registerModule("page", "export const page = 'shadow';") }
        assertTrue(shadow.message.orEmpty().contains("already evaluated"), "message was: ${shadow.message}")
        assertFailsWith<JsException> { engine.evaluateModule("export const page = 'twice';", name = "page") }
        engine.registerModule("lib", "export const lib = 1;")
        val registered = assertFailsWith<JsException> { engine.evaluateModule("export const lib = 2;", name = "lib") }
        assertTrue(registered.message.orEmpty().contains("already registered"), "message was: ${registered.message}")
        assertFailsWith<JsException> { engine.evaluateModule("export const = ;", name = "never") }
        engine.registerModule("never", "export const ok = 1;")
        assertEquals(0, engine.stats().liveRefs)
    }

    @Test
    fun topLevelAwaitSettlesWithinTheCallOrComesBackPending() = JsEngine().use { engine ->
        engine.evaluateModule("const x = await Promise.resolve(41); export const y = x + 1;", name = "settled").use {
            assertEquals(JsValue.Num(42), it.get("y"))
        }
        engine.registerFunction("ready") { JsValue.Undefined }
        val pending = engine.evaluateModule("await new Promise(r => { globalThis.__go = r; }); ready(); export const done = true;", name = "pending")
        assertTrue(pending.isPromise)
        pending.close()
        assertEquals(JsValue.Num(1), engine.evaluate("__go(); 1"))
        assertEquals(0, engine.stats().liveRefs)
    }

    @Test
    fun cyclicImportsAndHostFunctionsWorkInsideModules() = JsEngine().use { engine ->
        engine.registerFunction("tag") { JsValue.Str("#" + (it[0] as JsValue.Str).value) }
        engine.registerModule("ring-a", "import { b } from 'ring-b'; export const a = 'a'; export const viaB = () => b;")
        engine.registerModule("ring-b", "import { a } from 'ring-a'; export const b = 'b'; export const viaA = () => a;")
        engine.evaluateModule("import { viaB } from 'ring-a'; import { viaA } from 'ring-b'; export const r = tag(viaB() + viaA());", name = "ring").use {
            assertEquals(JsValue.Str("#ba"), it.get("r"))
        }
    }

    @Test
    fun namespaceExportsAreCallable() = JsEngine().use { engine ->
        engine.evaluateModule("export default function mount(n) { return n * 2; }", name = "page").use { page ->
            val mount = assertIs<JsRef>(page.get("default", ObjectTransport.REF))
            mount.use { assertEquals(JsValue.Num(8), it.call(JsValue.Num(4))) }
        }
    }

    @Test
    fun moduleLoaderSuppliesSourceOrBytecodeOnceAtFirstImport() {
        val asked = mutableListOf<String>()
        val core = JsBytecode.compile("import { from } from 'lazy'; export default from + '/bytecode';", "core", module = true)
        val loader = { name: String ->
            asked += name
            when (name) {
                "lazy" -> JsModuleSource.Text("export const from = 'loader'; export const url = import.meta.url;")
                "core" -> JsModuleSource.Bytecode(core)
                else -> null
            }
        }
        JsEngine(JsEngineConfig(moduleScheme = "app", moduleLoader = loader)).use { engine ->
            engine.evaluateModule("import core from 'core'; import { url } from 'lazy'; export const r = core + '@' + url;").use {
                assertEquals(JsValue.Str("loader/bytecode@app:lazy"), it.get("r"))
            }
            engine.evaluateModule("import { from } from 'lazy'; export default from;").use { assertEquals(JsValue.Str("loader"), it.get("default")) }
            assertEquals(listOf("core", "lazy"), asked)
            val shadow = assertFailsWith<JsException> { engine.registerModule("lazy", "export const from = 'shadow';") }
            assertTrue(shadow.message.orEmpty().contains("already loaded"), "message was: ${shadow.message}")
            assertEquals(0, engine.stats().liveRefs)
        }
    }

    @Test
    fun registeredModulesAreNeverAskedFromTheLoader() {
        var asked = 0
        JsEngine(JsEngineConfig(moduleLoader = { asked++; JsModuleSource.Text("export const v = 'loader';") })).use { engine ->
            engine.registerModule("m", "export const v = 'registered';")
            engine.evaluateModule("import { v } from 'm'; export default v;").use { assertEquals(JsValue.Str("registered"), it.get("default")) }
            assertEquals(0, asked)
        }
    }

    @Test
    fun loaderMissesAndFailuresSurfaceAtTheImport() {
        val wrongName = JsBytecode.compile("export const x = 1;", "other", module = true)
        val asked = mutableListOf<String>()
        val loader = { name: String ->
            asked += name
            when (name) {
                "boom" -> throw IllegalStateException("no network")
                "renamed" -> JsModuleSource.Bytecode(wrongName)
                "script" -> JsModuleSource.Bytecode(JsBytecode.compile("1 + 1", "script"))
                else -> null
            }
        }
        JsEngine(JsEngineConfig(moduleLoader = loader)).use { engine ->
            val missing = assertFailsWith<JsException> { engine.evaluateModule("import 'missing';") }
            assertEquals("ReferenceError: module 'missing' is not registered", missing.message)
            val boom = assertFailsWith<JsException> { engine.evaluateModule("import 'boom';") }
            assertEquals("Error: no network", boom.message)
            val renamed = assertFailsWith<JsException> { engine.evaluateModule("import 'renamed';") }
            assertTrue(renamed.message.orEmpty().contains("compiled as 'other'"), "message was: ${renamed.message}")
            val script = assertFailsWith<JsException> { engine.evaluateModule("import 'script';") }
            assertTrue(script.message.orEmpty().contains("not a module"), "message was: ${script.message}")
            // a miss or a failure spends no name: the next import asks again, and registering it later works
            assertFailsWith<JsException> { engine.evaluateModule("import 'missing';") }
            assertEquals(2, asked.count { it == "missing" })
            engine.registerModule("boom", "export const ok = true;")
            engine.evaluateModule("import { ok } from 'boom'; export default ok;").use { assertEquals(JsValue.Bool(true), it.get("default")) }
            assertEquals(1, asked.count { it == "boom" })
        }
    }

    @Test
    fun dynamicImportResolvesThroughTheLoader() {
        var asked = 0
        JsEngine(JsEngineConfig(moduleLoader = { asked++; if (it == "page/detail") JsModuleSource.Text("export const title = 'detail';") else null })).use { engine ->
            assertEquals(JsValue.Str("detail"), engine.evaluate("import('page/detail').then(m => m.title)"))
            assertEquals(1, asked)
            val e = assertFailsWith<JsException> { engine.evaluate("import('page/none')") }
            assertTrue(e.message.orEmpty().contains("'page/none' is not registered"), "message was: ${e.message}")
        }
    }

    @Test
    fun invalidSchemeIsRejected() {
        assertFailsWith<IllegalArgumentException> { JsEngineConfig(moduleScheme = "") }
        assertFailsWith<IllegalArgumentException> { JsEngineConfig(moduleScheme = "a:b") }
    }

    @Test
    fun runtimeRegistersModulesUnderTheLock() = runTest {
        val runtime = JsRuntime()
        try {
            runtime.registerModule("m", "export const v = 'from runtime';")
            val v = runtime.withEngine { evaluateModule("import { v } from 'm'; export default v;", name = "x").use { it.get("default") } }
            assertEquals(JsValue.Str("from runtime"), v)
        } finally {
            runtime.shutdown()
        }
    }
}

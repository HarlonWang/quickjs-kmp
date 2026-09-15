package wang.harlon.quickjs

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue

class JsPromiseTest {
    @Test
    fun microtasksRunAfterTheScriptBodyAndBeforeTheCallReturns() = JsEngine().use { engine ->
        assertEquals(
            JsValue.Str("sync"),
            engine.evaluate("var order = []; Promise.resolve().then(() => order.push('micro')); order.push('sync'); order.join()"),
        )
        assertEquals(JsValue.Str("sync,micro"), engine.evaluate("order.join()"))
    }

    @Test
    fun nestedCallFromHostFunctionDoesNotDrainEarly() = JsEngine().use { engine ->
        engine.registerFunction("inner") { engine.evaluate("order.push('inner')") }
        assertEquals(
            JsValue.Str("inner,sync"),
            engine.evaluate("var order = []; Promise.resolve().then(() => order.push('micro')); inner(); order.push('sync'); order.join()"),
        )
        assertEquals(JsValue.Str("inner,sync,micro"), engine.evaluate("order.join()"))
    }

    @Test
    fun asyncFunctionResultIsUnwrapped() = JsEngine().use { engine ->
        engine.evaluate("async function f() { await null; return 6 * 7; }")
        assertEquals(JsValue.Num(42), engine.evaluate("f()"))
        val f = engine.evaluate("f", objects = ObjectTransport.REF) as JsRef
        f.use { assertEquals(JsValue.Num(42), it.call()) }
    }

    @Test
    fun rejectedAsyncFunctionThrowsAndIsNotReportedAsUnhandled() {
        val reported = ArrayList<JsException>()
        JsEngine(JsEngineConfig(onUnhandledRejection = { reported += it })).use { engine ->
            val e = assertFailsWith<JsException> { engine.evaluate("(async () => { throw new Error('nope'); })()") }
            assertEquals("Error: nope", e.message)
            assertTrue(e.jsStack.orEmpty().isNotEmpty(), "stack was: ${e.jsStack}")
        }
        assertEquals(emptyList(), reported)
    }

    @Test
    fun pendingPromiseComesBackAsRef() = JsEngine().use { engine ->
        val pending = assertIs<JsRef>(engine.evaluate("var resolve; new Promise(r => { resolve = r; })"))
        assertTrue(pending.isPromise)
        pending.close()
        engine.evaluate("resolve(1)")
        assertEquals(0, engine.stats().liveRefs)
    }

    @Test
    fun unhandledRejectionReachesTheHandlerOnce() {
        val reported = ArrayList<JsException>()
        JsEngine(JsEngineConfig(onUnhandledRejection = { reported += it })).use { engine ->
            assertEquals(JsValue.Num(1), engine.evaluate("Promise.reject(new Error('lost')); 1"))
            assertEquals(JsValue.Num(2), engine.evaluate("var p = Promise.reject(new Error('caught')); p.catch(() => {}); 2"))
            assertEquals(JsValue.Num(3), engine.evaluate("3"))
        }
        assertEquals(listOf("Error: lost"), reported.map { it.message })
        assertTrue(reported[0].jsStack.orEmpty().isNotEmpty())
    }

    @Test
    fun unhandledRejectionFallsBackToTheLogger() {
        val lines = ArrayList<String>()
        JsEngine(JsEngineConfig(logger = { lines += it })).use { engine ->
            engine.evaluate("Promise.reject(new Error('lost')); 1")
        }
        assertEquals(listOf("Unhandled promise rejection: Error: lost"), lines)
    }

    @Test
    fun handlerExceptionsAreSwallowed() {
        JsEngine(JsEngineConfig(onUnhandledRejection = { throw IllegalStateException("handler broke") })).use { engine ->
            assertEquals(JsValue.Num(1), engine.evaluate("Promise.reject(1); 1"))
            assertEquals(JsValue.Num(2), engine.evaluate("2"))
        }
    }

    @Test
    fun interruptStopsAnEndlessPromiseChainAndDiscardsIt() = JsEngine().use { engine ->
        engine.registerFunction("stop") {
            engine.interrupt()
            JsValue.Undefined
        }
        val e = assertFailsWith<JsException> {
            engine.evaluate("var n = 0; function loop() { if (++n === 100) stop(); Promise.resolve().then(loop); } loop(); 1")
        }
        assertTrue(e.message.orEmpty().contains("interrupted"), "message was: ${e.message}")
        val n = (engine.evaluate("n") as JsValue.Num).value
        assertEquals(JsValue.Num(5), engine.evaluate("(async () => 5)()"))
        assertEquals(JsValue.Num(n), engine.evaluate("n"))
    }

    @Test
    fun scriptExceptionWinsOverJobFailures() = JsEngine().use { engine ->
        val e = assertFailsWith<JsException> { engine.evaluate("Promise.resolve().then(() => { throw new Error('in job'); }); null.x") }
        assertEquals("TypeError: cannot read property 'x' of null", e.message)
    }

    @Test
    fun jobsReenteringTheEngineDoNotClobberTheResult() = JsEngine().use { engine ->
        engine.registerFunction("inner") { engine.evaluate("'nested result'") }
        val e = assertFailsWith<JsException> { engine.evaluate("Promise.resolve().then(() => inner()); null.x", "clobber.js") }
        assertEquals("TypeError: cannot read property 'x' of null", e.message)
        assertTrue(e.jsStack.orEmpty().contains("clobber.js"), "stack was: ${e.jsStack}")
        assertEquals(JsValue.Str("kept"), engine.evaluate("Promise.resolve().then(() => inner()); 'kept'"))
    }
}

package wang.harlon.quickjs

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.time.TimeSource
import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.toKString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import platform.posix.fclose
import platform.posix.fgets
import platform.posix.fopen
import platform.posix.fread
import platform.posix.getenv
import platform.posix.pclose
import platform.posix.popen

/**
 * TinyUI acceptance (docs/roadmap.md M3): the reactivity bench of the TinyUI repo, run through
 * this SDK in the shape TinyUI uses it, one Kotlin entry per transaction with the patch stream
 * crossing into Kotlin as JSON. Opt-in: set TINYUI_BENCH_DIR to the `bench/` directory of a
 * TinyUI checkout that has `.engine-qjs/qjs` built; skipped otherwise. Numbers go to stdout.
 */
@OptIn(ExperimentalForeignApi::class)
class TinyUiBenchTest {
    private val benchDir: String? = getenv("TINYUI_BENCH_DIR")?.toKString()

    private fun benchFile(name: String): String = readFile("$benchDir/$name")

    private fun qjsVerifyStream(scenario: String): List<String> {
        val dir = shellQuote(benchDir!!)
        val cmd = "$dir/.engine-qjs/qjs --std -I $dir/harness.js -I $dir/signal.js $dir/run.js $scenario verify"
        return runCommand(cmd).filter { !it.startsWith("RESULT ") }
    }

    /** popen goes through the shell; a single-quoted word only needs its own quotes escaped. */
    private fun shellQuote(path: String): String = "'" + path.replace("'", "'\\''") + "'"

    private fun newEngine(lines: MutableList<String>): JsEngine =
        JsEngine(JsEngineConfig(memoryLimit = 64L * 1024 * 1024, logger = { lines += it })).also { engine ->
            engine.evaluate(benchFile("harness.js"), "harness.js")
            engine.evaluate(benchFile("signal.js"), "signal.js")
            engine.evaluate("var scriptArgs = ['run.js']; var gc = function () {};")
        }

    @Test
    fun patchStreamThroughTheBridgeMatchesQjs() {
        val dir = benchDir ?: return println("TinyUiBenchTest skipped: TINYUI_BENCH_DIR not set")
        for (scenario in listOf("S1", "S2", "S3", "S4")) {
            val expected = qjsVerifyStream(scenario)
            // harness-driven, patch JSON crossing through a host function
            val crossed = ArrayList<String>()
            newEngine(ArrayList()).use { engine ->
                engine.registerFunction("__hostApply") { args -> crossed += (args[0] as JsValue.Str).value; JsValue.Undefined }
                engine.evaluate("__host = { apply: function (json, count) { __flushCount++; __patchCount += count; __patchBytes += json.length; __hostApply(json); } };")
                engine.evaluate("runScenario('$scenario', true)")
            }
            assertEquals(expected, crossed, "$scenario: harness through the bridge differs from qjs")
            // Kotlin-driven transactions must produce the same stream
            val driven = ArrayList<String>()
            newEngine(ArrayList()).use { engine ->
                engine.registerFunction("__hostApply") { args -> driven += (args[0] as JsValue.Str).value; JsValue.Undefined }
                engine.evaluate("__host = { apply: function (json, count) { __hostApply(json); } };")
                driveScenario(engine, scenario, verify = true)
            }
            assertEquals(expected, driven, "$scenario: Kotlin-driven transactions differ from qjs")
        }
        println("TinyUiBenchTest: patch streams match qjs for S1..S4 ($dir)")
    }

    @Test
    fun bridgeShapedScenarios() {
        benchDir ?: return println("TinyUiBenchTest skipped: TINYUI_BENCH_DIR not set")
        val rows = ArrayList<String>()
        for (scenario in listOf("S1", "S2", "S3", "S4")) {
            val jsOnly = median(5) { jsOnlyRun(scenario) }
            val crossing = median(5) { bridgedRun(scenario, parse = false).ms }
            val parsed = List(5) { bridgedRun(scenario, parse = true) }.sortedBy { it.ms }[2]
            rows += "| $scenario | ${jsOnly.oneDecimal()} | ${crossing.oneDecimal()} | ${parsed.ms.oneDecimal()} | ${parsed.patches} | ${parsed.bytes} | ${parsed.memory / 1024} |"
        }
        println("| scenario | js-only ms | + crossing ms | + JSON parse ms | patches | bytes | memory KiB |")
        println("|---|---:|---:|---:|---:|---:|---:|")
        rows.forEach(::println)
    }

    private class BridgedRun(val ms: Double, val patches: Long, val bytes: Long, val memory: Long)

    /** Kotlin-driven transactions; the host function only receives the JSON, or also parses it. */
    private fun bridgedRun(scenario: String, parse: Boolean): BridgedRun {
        var count = 0L
        var size = 0L
        return newEngine(ArrayList()).use { engine ->
            engine.registerFunction("__hostApply") { args ->
                val json = (args[0] as JsValue.Str).value
                size += json.length
                if (parse) count += Json.parseToJsonElement(json).jsonArray.size
                JsValue.Undefined
            }
            engine.evaluate("__host = { apply: function (json, count) { __hostApply(json); } };")
            val ms = driveScenario(engine, scenario, verify = false)
            BridgedRun(ms, count, size, engine.stats().memoryUsed)
        }
    }

    /** The harness's own timing: everything inside the engine, __host.apply is the JS stub. */
    private fun jsOnlyRun(scenario: String): Double {
        val lines = ArrayList<String>()
        newEngine(lines).use { engine -> engine.evaluate("runScenario('$scenario', false)") }
        val result = lines.last { it.startsWith("RESULT ") }.removePrefix("RESULT ")
        return Regex("\"ms\":([0-9.]+)").find(result)!!.groupValues[1].toDouble()
    }

    /**
     * Mirrors harness.js runScenario, but every transaction is one Kotlin entry into the engine:
     * a JS helper does the update and the flush, and the patch JSON crosses back through __hostApply.
     * Returns the milliseconds spent in the timed section.
     */
    private fun driveScenario(engine: JsEngine, scenario: String, verify: Boolean): Double {
        val n = if (verify) 20 else 1000
        engine.evaluate(
            """
            var __items = makeItems($n);
            function __mount() { app.mount(__items); app.flush(); }
            function __tx2(i, k) { app.setVer(i, k); app.flush(); }
            function __tx3(r, m) { for (var i = 0; i < m; i++) { var idx = (i * ($n / m) + r) % $n; if ((i & 3) === 0) app.setDone(idx, (r & 1) === 1); else app.setVer(idx, r); } app.flush(); }
            function __tx4() { app.setItems([]); app.flush(); app.setItems(__items); app.flush(); }
            """.trimIndent(),
        )
        val mount = engine.evaluate("__mount", objects = ObjectTransport.REF) as JsRef
        val tx2 = engine.evaluate("__tx2", objects = ObjectTransport.REF) as JsRef
        val tx3 = engine.evaluate("__tx3", objects = ObjectTransport.REF) as JsRef
        val tx4 = engine.evaluate("__tx4", objects = ObjectTransport.REF) as JsRef
        val mark = TimeSource.Monotonic.markNow()
        var start = mark
        when (scenario) {
            "S1" -> {
                start = TimeSource.Monotonic.markNow()
                mount.call()
            }
            "S2" -> {
                mount.call()
                val k = if (verify) 10 else 200000
                start = TimeSource.Monotonic.markNow()
                for (i in 1..k) tx2.call(JsValue.Num(i % n), JsValue.Num(i))
            }
            "S3" -> {
                mount.call()
                val r = if (verify) 3 else 2000
                val m = if (verify) 5 else 100
                start = TimeSource.Monotonic.markNow()
                for (i in 1..r) tx3.call(JsValue.Num(i), JsValue.Num(m))
            }
            "S4" -> {
                mount.call()
                val r = if (verify) 2 else 30
                start = TimeSource.Monotonic.markNow()
                repeat(r) { tx4.call() }
            }
        }
        val elapsed = start.elapsedNow().inWholeMicroseconds / 1000.0
        listOf(mount, tx2, tx3, tx4).forEach { it.close() }
        return elapsed
    }

    private fun Double.oneDecimal(): String = (kotlin.math.round(this * 10) / 10).toString()

    private fun median(runs: Int, block: () -> Double): Double = List(runs) { block() }.sorted()[runs / 2]

    private fun readFile(path: String): String {
        val f = fopen(path, "rb") ?: error("cannot open $path")
        try {
            val chunks = ArrayList<ByteArray>()
            memScoped {
                val buf = allocArray<ByteVar>(65536)
                while (true) {
                    val n = fread(buf, 1u, 65536u, f).toInt()
                    if (n <= 0) break
                    chunks += buf.readBytes(n)
                }
            }
            return chunks.fold(ByteArray(0)) { acc, c -> acc + c }.decodeToString()
        } finally {
            fclose(f)
        }
    }

    private fun runCommand(cmd: String): List<String> {
        val p = popen(cmd, "r") ?: error("cannot run $cmd")
        val lines = ArrayList<String>()
        memScoped {
            val buf = allocArray<ByteVar>(1 shl 20)
            while (fgets(buf, 1 shl 20, p) != null) lines += buf.toKString().trimEnd('\n')
        }
        pclose(p)
        return lines
    }
}

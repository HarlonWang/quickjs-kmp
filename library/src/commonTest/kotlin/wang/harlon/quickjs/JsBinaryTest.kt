package wang.harlon.quickjs

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertTrue

class JsBinaryTest {
    @Test
    fun bigIntCrossesAsDecimalText() = JsEngine().use { engine ->
        assertEquals(JsValue.BigInt("18446744073709551616"), engine.evaluate("2n ** 64n"))
        assertEquals(JsValue.BigInt("-7"), engine.evaluate("-7n"))
        assertEquals(JsValue.BigInt("1"), engine.evaluate("1n", objects = ObjectTransport.REF))
        engine.registerFunction("echo") { it[0] }
        assertEquals(JsValue.Str("bigint:18446744073709551617"), engine.evaluate("typeof echo(5n) + ':' + (echo(2n ** 64n) + 1n)"))
        engine.registerFunction("min") { JsValue.BigInt(Long.MIN_VALUE) }
        assertEquals(JsValue.Bool(true), engine.evaluate("min() === -9223372036854775808n"))
    }

    @Test
    fun malformedBigIntIsRejected() {
        assertFailsWith<IllegalArgumentException> { JsValue.BigInt("12a") }
        assertFailsWith<IllegalArgumentException> { JsValue.BigInt("") }
        assertFailsWith<IllegalArgumentException> { JsValue.BigInt("-") }
        assertFailsWith<IllegalArgumentException> { JsValue.BigInt("1-2") }
        JsValue.BigInt("-0")
        JsValue.BigInt("00012")
    }

    @Test
    fun binaryCrossesAsBytes() = JsEngine().use { engine ->
        assertEquals(JsValue.Bytes(byteArrayOf(1, 2, 3)), engine.evaluate("new Uint8Array([1, 2, 3])"))
        assertEquals(JsValue.Bytes(byteArrayOf(1, 2, 3)), engine.evaluate("new Uint8Array([9, 1, 2, 3, 9]).subarray(1, 4)"))
        assertEquals(JsValue.Bytes(byteArrayOf(-1)), engine.evaluate("new Int8Array([-1])"))
        assertEquals(16, (engine.evaluate("new Float64Array(2)") as JsValue.Bytes).value.size)
        assertEquals(JsValue.Bytes(ByteArray(0)), engine.evaluate("new ArrayBuffer(0)"))
        assertEquals(4, (engine.evaluate("new SharedArrayBuffer(4)") as JsValue.Bytes).value.size)
        assertIs<JsValue.Json>(engine.evaluate("new DataView(new ArrayBuffer(4))"))
        engine.registerFunction("bytes") { JsValue.Bytes(byteArrayOf(5, 6, -1)) }
        assertEquals(JsValue.Str("ArrayBuffer:3:5,6,255"), engine.evaluate("var b = bytes(); b.constructor.name + ':' + b.byteLength + ':' + new Uint8Array(b).join()"))
    }

    @Test
    fun refTransportHandsBinaryOutAsRefs() = JsEngine().use { engine ->
        val ref = assertIs<JsRef>(engine.evaluate("new Uint8Array([7, 8])", objects = ObjectTransport.REF))
        ref.use {
            assertFalse(it.isArray)
            assertEquals(JsValue.Num(8), it.get(1))
            assertEquals(JsValue.Bytes(byteArrayOf(7, 8)), engine.evaluate("(function (a) { return a; })", objects = ObjectTransport.REF).let { f -> (f as JsRef).use { fn -> fn.call(it) } })
        }
    }

    @Test
    fun bytesEqualityIsByContent() {
        assertEquals(JsValue.Bytes(byteArrayOf(1)), JsValue.Bytes(byteArrayOf(1)))
        assertEquals(JsValue.Bytes(byteArrayOf(1)).hashCode(), JsValue.Bytes(byteArrayOf(1)).hashCode())
        assertTrue(JsValue.Bytes(byteArrayOf(1)) != JsValue.Bytes(byteArrayOf(2)))
        assertEquals("Bytes(2 bytes)", JsValue.Bytes(byteArrayOf(1, 2)).toString())
    }

    @Test
    fun serializationDecodesBigIntAndBytes() = JsEngine().use { engine ->
        assertEquals(Long.MAX_VALUE, engine.evaluate("9223372036854775807n").decode<Long>())
        assertEquals(42, engine.evaluate("42n").decode<Int>())
        assertContentEquals(byteArrayOf(1, -1), engine.evaluate("new Uint8Array([1, 255])").decode<ByteArray>())
        assertEquals(listOf(1, -1), engine.evaluate("new Uint8Array([1, 255])").decode<List<Int>>())
    }
}

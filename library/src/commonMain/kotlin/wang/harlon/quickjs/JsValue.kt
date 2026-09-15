package wang.harlon.quickjs

/**
 * A JavaScript value crossing the engine boundary. Primitives are carried as-is, `BigInt` as
 * decimal text ([BigInt]), binary buffers as a copy of their bytes ([Bytes]); other objects and
 * arrays travel as JSON text ([Json]) or as live handles ([JsRef]).
 */
sealed interface JsValue {
    object Undefined : JsValue {
        override fun toString(): String = "undefined"
    }

    object Null : JsValue {
        override fun toString(): String = "null"
    }

    data class Bool(val value: Boolean) : JsValue

    data class Num(val value: Double) : JsValue {
        constructor(value: Int) : this(value.toDouble())
    }

    data class Str(val value: String) : JsValue

    /**
     * An object or array. [json] is null when the value cannot be serialized,
     * for example a function.
     */
    data class Json(val json: String?) : JsValue

    /** A `BigInt`, carried as decimal text so no precision is lost. */
    data class BigInt(val value: String) : JsValue {
        constructor(value: Long) : this(value.toString())

        init {
            require(value.isNotEmpty() && value.all { it.isDigit() || it == '-' } && value.lastIndexOf('-') <= 0 && value != "-") {
                "'$value' is not an integer"
            }
        }
    }

    /**
     * Binary data. Leaving the engine it is a copy of an `ArrayBuffer`, `SharedArrayBuffer` or typed
     * array (the viewed range only); entering the engine it becomes an `ArrayBuffer`.
     */
    class Bytes(val value: ByteArray) : JsValue {
        override fun equals(other: Any?): Boolean = other is Bytes && value.contentEquals(other.value)
        override fun hashCode(): Int = value.contentHashCode()
        override fun toString(): String = "Bytes(${value.size} bytes)"
    }
}

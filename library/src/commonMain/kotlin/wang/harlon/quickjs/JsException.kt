package wang.harlon.quickjs

/**
 * A JavaScript exception that escaped to the host. [message] is the result of the
 * engine's `toString()` on the thrown value; [jsStack] is the `stack` property for Error objects.
 */
class JsException(message: String, val jsStack: String? = null) : RuntimeException(message)

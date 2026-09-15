package wang.harlon.quickjs

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class QuickJsTest {
    @Test
    fun sdkVersionIsPresent() {
        assertTrue(QuickJs.sdkVersion.isNotBlank())
    }

    @Test
    fun upstreamCommitIsFullSha() {
        assertEquals(40, QuickJs.upstreamCommit.length)
        assertTrue(QuickJs.upstreamCommit.all { it in '0'..'9' || it in 'a'..'f' })
    }
}

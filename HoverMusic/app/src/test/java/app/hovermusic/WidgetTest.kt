package app.hovermusic

import org.junit.Assert.assertEquals
import org.junit.Test

class WidgetTest {
    @Test fun lesbareSchrift() {
        assertEquals(0xFF14141A.toInt(), WidgetStyle.contentColor(0xFFF5F5F8.toInt()))
        assertEquals(0xFFFFFFFF.toInt(), WidgetStyle.contentColor(0xFF191922.toInt()))
    }
}

import com.android.uiautomator.core.Configurator;
import com.android.uiautomator.core.UiObject;
import com.android.uiautomator.core.UiSelector;
import com.android.uiautomator.testrunner.UiAutomatorTestCase;

/** A temporary adb-shell UIAutomator test jar, not an Android application. */
public class ShellUi extends UiAutomatorTestCase {
    private static class Payload extends UiObject {
        Payload() { super(new UiSelector().resourceId("no.nordicsemi.android.mcp:id/value")
            .className("android.widget.EditText")); }
        boolean replaceHex(String hex) throws Exception {
            Configurator.getInstance().setKeyInjectionDelay(80);
            return setText(hex);
        }
    }
    public void testDump() throws Exception {
        Configurator.getInstance().setWaitForIdleTimeout(0);
        String hex = getParams().getString("hex");
        if (hex != null) {
            assertTrue("Invalid hex", hex.matches("([0-9a-fA-F]{2}){0,64}"));
            assertTrue("Not a GATT write dialog", new UiObject(new UiSelector()
                .resourceId("android:id/title").text("Write value")).exists());
            assertTrue("Set payload failed", new Payload().replaceHex(hex));
            Thread.sleep(300);
        }
        String output = getParams().getString("output");
        assertTrue("Invalid output name", output.matches("wch-nrf-[0-9]+-[0-9]+\\.xml"));
        java.io.File file = new java.io.File("/data/local/tmp", output);
        if (file.exists()) assertTrue("Old XML could not be removed", file.delete());
        // Window transitions can briefly have no accessibility root. Retry
        // only the read; never repeat the payload input above.
        for (int i = 0; i < 5; i++) {
            getUiDevice().dumpWindowHierarchy(output);
            if (file.isFile() && file.length() > 0) return;
            Thread.sleep(200);
        }
        fail("UI XML missing");
    }
}

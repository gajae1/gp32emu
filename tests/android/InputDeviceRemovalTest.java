package org.gp32emu.app;

import android.hardware.input.InputManager.InputDeviceListener;
import java.lang.reflect.Field;
import java.util.Map;

/** Device-free JVM regression for the real input owner. Compile with android.jar,
 * InputState.java and KeyBindings.java, then run this class with the same jar.
 * SDK event constructors are stubs on a host JVM, so only fixture setup uses
 * reflection to seed the key/axis maps that key() and motion() normally fill.
 * The removal callback and resulting mask are production code, not a fake.
 * This does not exercise Android's listener registration or physical hotplug. */
public final class InputDeviceRemovalTest {
    private static int failures;

    @SuppressWarnings("unchecked")
    private static <K> Map<K, Integer> held(InputState state, String field) throws Exception {
        Field member = InputState.class.getDeclaredField(field);
        member.setAccessible(true);
        return (Map<K, Integer>)member.get(state);
    }

    private static void key(InputState state, int device, int code, int mask) throws Exception {
        InputDeviceRemovalTest.<Long>held(state, "keys")
                .put(((long)device << 32) | (code & 0xffffffffL), mask);
    }

    private static void axis(InputState state, int device, int mask) throws Exception {
        InputDeviceRemovalTest.<Integer>held(state, "axes").put(device, mask);
    }

    private static void remove(InputState state, int device) {
        // The baseline has no listener, so no removal can reach its held maps.
        // Keep that baseline executable: assert the stale state, not a missing
        // method or a compilation error, in the checks below.
        Object receiver = state;
        if (receiver instanceof InputDeviceListener)
            ((InputDeviceListener)receiver).onInputDeviceRemoved(device);
    }

    private static void expect(InputState state, int want, String label) {
        int got = state.mask();
        if (got != want) {
            System.err.printf("FAIL: %s: mask=%04x expected=%04x%n", label, got, want);
            ++failures;
        }
    }

    public static void main(String[] args) throws Exception {
        InputState state = new InputState();
        state.setTouch(InputState.SELECT);
        key(state, 7, 96, InputState.A);
        axis(state, 7, InputState.LEFT);
        key(state, 8, 97, InputState.B);
        axis(state, 8, InputState.DOWN);
        key(state, -1, 54, InputState.A);
        key(state, -1, 29, InputState.L);
        expect(state, InputState.SELECT | InputState.A | InputState.B | InputState.L
                | InputState.LEFT | InputState.DOWN, "fixture union");
        remove(state, 99);
        expect(state, InputState.SELECT | InputState.A | InputState.B | InputState.L
                | InputState.LEFT | InputState.DOWN, "unknown device preserves all sources");
        remove(state, 7);
        expect(state, InputState.SELECT | InputState.A | InputState.B | InputState.L
                | InputState.DOWN, "disconnect clears only that pad's key and axis");
        remove(state, 8);
        expect(state, InputState.SELECT | InputState.A | InputState.L,
                "second disconnect preserves keyboard and touch");
        remove(state, -1);
        expect(state, InputState.SELECT, "signed device id clears all its keys");
        remove(state, -1);
        expect(state, InputState.SELECT, "repeated removal is idempotent");
        key(state, 7, 97, InputState.B);
        expect(state, InputState.SELECT | InputState.B, "reused device id has no stale holds");
        remove(state, 7);
        key(state, 0, 103, InputState.R);
        axis(state, 0, InputState.UP);
        remove(state, 0);
        expect(state, InputState.SELECT, "device zero releases without clearing touch");
        state.clear();
        expect(state, 0, "lifecycle clear still releases every source");
        if (failures != 0) throw new AssertionError(failures + " removal checks failed");
        System.out.println("PASS: device removal releases owned keys/axes; other sources and id reuse remain correct");
    }
}

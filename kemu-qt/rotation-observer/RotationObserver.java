package org.kemu.rotation;

import android.content.res.Configuration;
import android.graphics.Rect;
import android.os.IBinder;
import android.view.IDisplayWindowListener;
import android.view.IRotationWatcher;
import java.lang.reflect.Method;
import java.util.List;

public final class RotationObserver extends IRotationWatcher.Stub {
    private int lastRotation = -1;
    private Object displayManager;
    private Method getDisplayInfo;
    private String lastDisplay;

    @Override public synchronized void onRotationChanged(int rotation) {
        if (rotation < 0 || rotation > 3 || rotation == lastRotation) return;
        lastRotation = rotation;
        System.out.println("KEMU_ROTATION_V1 " + rotation);
        System.out.flush();
    }

    // Rotation and display size changes both change the display configuration.
    private final class DisplayListener extends IDisplayWindowListener.Stub {
        @Override public void onDisplayConfigurationChanged(int displayId, Configuration newConfig) {
            if (displayId == 0) publishDisplay();
        }

        @Override public void onDisplayAdded(int displayId) {}
        @Override public void onDisplayRemoved(int displayId) {}
        @Override public void onFixedRotationStarted(int displayId, int newRotation) {}
        @Override public void onFixedRotationFinished(int displayId) {}
        @Override public void onKeepClearAreasChanged(int displayId, List<Rect> restricted,
                List<Rect> unrestricted) {}
        @Override public void onDesktopModeEligibleChanged(int displayId) {}
        @Override public void onDisplayAddSystemDecorations(int displayId) {}
        @Override public void onDisplayRemoveSystemDecorations(int displayId) {}
        @Override public void onDisplayAnimationsDisabledChanged(int displayId, boolean disabled) {}
    }

    // Publishes the rotation and logical size of display 0 when they change.
    private synchronized void publishDisplay() {
        try {
            Object info = getDisplayInfo.invoke(displayManager, 0);
            if (info == null) return;
            Class<?> infoClass = info.getClass();
            String line = "KEMU_DISPLAY_V1 " + infoClass.getField("rotation").getInt(info) + " "
                    + infoClass.getField("logicalWidth").getInt(info) + " "
                    + infoClass.getField("logicalHeight").getInt(info);
            if (line.equals(lastDisplay)) return;
            lastDisplay = line;
            System.out.println(line);
            System.out.flush();
        } catch (Exception error) {
            System.err.println("Kemu display observer failed: " + error);
        }
    }

    private static IBinder getService(String name) throws Exception {
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, name);
        if (binder == null) throw new IllegalStateException(name + " service unavailable");
        return binder;
    }

    private void watchDisplay(Object windowManager, Class<?> windowManagerClass) {
        try {
            IBinder binder = getService("display");
            synchronized (this) {
                displayManager = Class.forName("android.hardware.display.IDisplayManager$Stub")
                        .getMethod("asInterface", IBinder.class).invoke(null, binder);
                getDisplayInfo = Class.forName("android.hardware.display.IDisplayManager")
                        .getMethod("getDisplayInfo", int.class);
            }
            windowManagerClass.getMethod("registerDisplayWindowListener", IDisplayWindowListener.class)
                    .invoke(windowManager, new DisplayListener());
            publishDisplay();
        } catch (Exception error) {
            // Rotation does not depend on the display state.
            System.err.println("Kemu display observer unavailable: " + error);
        }
    }

    private void run() throws Exception {
        IBinder binder = getService("window");
        Object windowManager = Class.forName("android.view.IWindowManager$Stub")
                .getMethod("asInterface", IBinder.class).invoke(null, binder);
        Class<?> managerClass = Class.forName("android.view.IWindowManager");
        Method watch = managerClass.getMethod("watchRotation", IRotationWatcher.class, int.class);
        Method remove = managerClass.getMethod("removeRotationWatcher", IRotationWatcher.class);
        IBinder.DeathRecipient death = () -> System.exit(1);
        binder.linkToDeath(death, 0);
        try {
            // Publish the initial value before any callbacks queued during registration.
            synchronized (this) {
                onRotationChanged((Integer) watch.invoke(windowManager, this, 0));
            }
            watchDisplay(windowManager, managerClass);
            // The ADB stream owns this process; disconnecting its stdin ends the observer.
            while (System.in.read() != -1) {}
        } finally {
            try {
                remove.invoke(windowManager, this);
            } finally {
                binder.unlinkToDeath(death, 0);
            }
        }
    }

    public static void main(String[] args) {
        int result = 0;
        try {
            new RotationObserver().run();
        } catch (Exception error) {
            System.err.println("Kemu rotation observer failed: " + error);
            result = 1;
        }
        // Binder worker threads must not outlive the stream.
        System.exit(result);
    }
}

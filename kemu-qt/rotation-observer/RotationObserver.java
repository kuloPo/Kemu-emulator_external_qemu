package org.kemu.rotation;

import android.os.IBinder;
import android.view.IRotationWatcher;
import java.lang.reflect.Method;

public final class RotationObserver extends IRotationWatcher.Stub {
    private int lastRotation = -1;

    @Override public synchronized void onRotationChanged(int rotation) {
        if (rotation < 0 || rotation > 3 || rotation == lastRotation) return;
        lastRotation = rotation;
        System.out.println("KEMU_ROTATION_V1 " + rotation);
        System.out.flush();
    }

    private void run() throws Exception {
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, "window");
        if (binder == null) throw new IllegalStateException("Window service unavailable");
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

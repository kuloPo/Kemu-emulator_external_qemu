package android.view;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;

// Compile-time declarations only. Android supplies the Binder implementation.
public interface IRotationWatcher extends IInterface {
    void onRotationChanged(int rotation) throws RemoteException;

    abstract class Stub extends Binder implements IRotationWatcher {
        @Override public IBinder asBinder() { throw new UnsupportedOperationException(); }
    }
}

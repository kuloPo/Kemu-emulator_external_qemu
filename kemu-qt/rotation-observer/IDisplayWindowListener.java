package android.view;

import android.content.res.Configuration;
import android.graphics.Rect;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.RemoteException;
import java.util.List;

// Compile-time declarations only. Android supplies the Binder implementation.
public interface IDisplayWindowListener extends IInterface {
    void onDisplayAdded(int displayId) throws RemoteException;
    void onDisplayConfigurationChanged(int displayId, Configuration newConfig) throws RemoteException;
    void onDisplayRemoved(int displayId) throws RemoteException;
    void onFixedRotationStarted(int displayId, int newRotation) throws RemoteException;
    void onFixedRotationFinished(int displayId) throws RemoteException;
    void onKeepClearAreasChanged(int displayId, List<Rect> restricted, List<Rect> unrestricted)
            throws RemoteException;
    void onDesktopModeEligibleChanged(int displayId) throws RemoteException;
    void onDisplayAddSystemDecorations(int displayId) throws RemoteException;
    void onDisplayRemoveSystemDecorations(int displayId) throws RemoteException;
    void onDisplayAnimationsDisabledChanged(int displayId, boolean disabled) throws RemoteException;

    abstract class Stub extends Binder implements IDisplayWindowListener {
        @Override public IBinder asBinder() { throw new UnsupportedOperationException(); }
    }
}

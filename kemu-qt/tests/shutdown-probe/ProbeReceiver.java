package com.kemu.shutdownprobe;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import java.util.Arrays;

// Writes real SharedPreferences without a test-side sync before host close.
public final class ProbeReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        SharedPreferences prefs = context.getSharedPreferences("probe", Context.MODE_PRIVATE);
        String token = intent.getStringExtra("token");
        char[] data = new char[1024 * 1024];
        Arrays.fill(data, 'x');
        String payload = new String(data);
        if ("com.kemu.shutdownprobe.WRITE".equals(intent.getAction())) {
            boolean saved = prefs.edit().clear().putString("token", token)
                    .putString("payload", payload).commit();
            setResultCode(saved ? 0 : 1);
            setResultData(saved ? token : "WRITE_FAILED");
        } else {
            boolean valid = token != null && token.equals(prefs.getString("token", ""))
                    && payload.equals(prefs.getString("payload", ""));
            setResultCode(valid ? 0 : 1);
            setResultData(valid ? token : "DATA_LOST");
        }
    }
}

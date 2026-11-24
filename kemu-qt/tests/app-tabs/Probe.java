package org.kemu.tests.tabs;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;

public final class Probe extends Activity {
    private final Handler handler = new Handler();
    private int draws;
    private int ticks;
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            Log.i("KemuTabProbe", getPackageName() + " display=" + getDisplay().getDisplayId()
                    + " task=" + getTaskId() + " pid=" + android.os.Process.myPid()
                    + " focus=" + hasWindowFocus() + " ticks=" + ++ticks + " draws=" + draws
                    + " uptime=" + SystemClock.uptimeMillis());
            handler.postDelayed(this, 1000);
        }
    };

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        EditText text = new EditText(this);
        text.setSingleLine();
        text.setHint(getPackageName());
        column.addView(text);
        column.addView(new View(this) {
            final Paint paint = new Paint();
            @Override protected void onDraw(Canvas canvas) {
                canvas.drawColor(getPackageName().endsWith("wide") ? 0xff14584c : 0xff304c83);
                paint.setColor(0xffffffff);
                paint.setTextSize(42);
                canvas.drawText("Task " + getTaskId() + "   frame " + ++draws, 48, 80, paint);
                canvas.drawCircle(32 + draws % Math.max(1, getWidth() - 64), getHeight() / 2, 24, paint);
                postInvalidateOnAnimation();
            }
            @Override public boolean onTouchEvent(MotionEvent e) {
                Log.i("KemuTabProbe", getPackageName() + " touch=" + e.getActionMasked()
                        + " pointers=" + e.getPointerCount() + " x=" + e.getX() + " y=" + e.getY());
                return true;
            }
        });
        setContentView(column);
        handler.post(tick);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        Log.i("KemuTabProbe", getPackageName() + " key=" + event.getKeyCode() + " action=" + event.getAction());
        return super.dispatchKeyEvent(event);
    }

    @Override public void onDestroy() {
        handler.removeCallbacks(tick);
        super.onDestroy();
    }
}

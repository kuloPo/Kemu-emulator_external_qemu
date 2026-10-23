package org.kemu.test.rotation;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;

public class RotationActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setRequestedOrientation(getIntent().getIntExtra("orientation", 1));
        setContentView(new View(this) {
            final Paint paint = new Paint();
            @Override public void onDraw(Canvas canvas) {
                canvas.drawColor(Color.WHITE);
                paint.setColor(Color.RED);
                canvas.drawRect(0, 0, getWidth() / 2f, getHeight() / 2f, paint);
                paint.setColor(Color.BLUE);
                canvas.drawRect(getWidth() / 2f, 0, getWidth(), getHeight() / 2f, paint);
                paint.setColor(Color.GREEN);
                canvas.drawRect(0, getHeight() / 2f, getWidth() / 2f, getHeight(), paint);
                paint.setColor(Color.YELLOW);
                canvas.drawRect(getWidth() / 2f, getHeight() / 2f, getWidth(), getHeight(), paint);
                paint.setColor(Color.BLACK);
                paint.setTextSize(70);
                canvas.drawText("TOP LEFT", 30, 130, paint);
                canvas.drawText("BOTTOM", 30, getHeight() - 60, paint);
            }
            @Override public boolean onTouchEvent(MotionEvent event) {
                if (event.getAction() == MotionEvent.ACTION_UP) {
                    Log.i("RotationProbe", "TOUCH " + event.getX() / getWidth() + " " + event.getY() / getHeight());
                }
                return true;
            }
        });
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
}

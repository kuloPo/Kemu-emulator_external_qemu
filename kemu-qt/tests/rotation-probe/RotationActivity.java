package org.kemu.test.rotation;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Point;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;

public class RotationActivity extends Activity implements SensorEventListener {
    private final Handler handler = new Handler();
    private final float[] acceleration = new float[3];
    private float gyroPeak;
    private SensorManager sensors;
    private final Runnable report = new Runnable() {
        @Override public void run() {
            Point size = new Point();
            getWindowManager().getDefaultDisplay().getRealSize(size);
            Log.i("RotationProbe", "STATE " + acceleration[0] + " " + acceleration[1]
                    + " " + acceleration[2] + " GYRO " + gyroPeak + " ROT "
                    + getWindowManager().getDefaultDisplay().getRotation() + " SIZE " + size.x + " " + size.y);
            gyroPeak = 0;
            handler.postDelayed(this, 200);
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        sensors = (SensorManager) getSystemService(SENSOR_SERVICE);
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

    @Override public void onResume() {
        super.onResume();
        sensors.registerListener(this, sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER), SensorManager.SENSOR_DELAY_GAME);
        sensors.registerListener(this, sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE), SensorManager.SENSOR_DELAY_GAME);
        handler.post(report);
    }

    @Override public void onPause() {
        handler.removeCallbacks(report);
        sensors.unregisterListener(this);
        super.onPause();
    }

    @Override public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            System.arraycopy(event.values, 0, acceleration, 0, 3);
        } else {
            gyroPeak = Math.max(gyroPeak, Math.abs(event.values[2]));
        }
    }

    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
}

package com.fde.fusionwindowmanager;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.util.Log;
import android.view.View;

import androidx.annotation.NonNull;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class WindowManager implements AWindowManagerInterface {

    private static final String TAG = "WindowManager";
    public static boolean ALREADY_SET_SCREEN_SIZE;

    // Used to load the 'fusionwindowmanager' library on application startup.
    static {
        System.loadLibrary("fusionwindowmanager");
    }

    HandlerThread mThread;
    static Handler mHandler;

    private static WeakReference<Context> contextReference;
    public static boolean isConnected;
    public static final float DECORCATIONVIEW_HEIGHT = 42;
    public static final int MSG_START_WM = 1;
    public static final int MSG_STOP_WM = 2;
    public static List<WindowAttribute> PERFORM_WINDOW_LIST = new ArrayList<>();
    public Set<Long> WINDOW_XIDS = new HashSet<>();
    private String display;

    public WindowManager() {
        mThread = new HandlerThread("WM");
        mThread.start();
        mHandler = new TaskHandler(mThread.getLooper());
    }

    public WindowManager(WeakReference<Context> activityWeakReference) {
        this.contextReference = activityWeakReference;
        mThread = new HandlerThread("WM");
        mThread.start();
        mHandler = new TaskHandler(mThread.getLooper());
    }


    public void startWindowManager(String displayGlobalParam) {
        this.display = displayGlobalParam;
        Message msg = Message.obtain();
        msg.what = MSG_START_WM;
        mHandler.sendMessage(msg);
    }

    public static boolean isConnected() {
        return isConnected;
    }

    public void stopWindowManager() {
        disconnect2Server();
    }

    /**
     * these native methods that is implemented by the 'fusionwindowmanager' native library,
     * which is packaged with this application.
     */
    public native void createXWindow();

    public static native int connect2Server(String display);

    public native int configureWindow(long window, int x, int y, int width, int height);

    public native int moveWindow(long window, int x, int y);

    public native int resizeWindow(long window, int width, int height);

    public native int closeWindow(long window);
    public native int raiseWindow(long window);

    public native int sendClipText(String cliptext);
    public native int disconnect2Server();

    public static void  getWindowIconFromManager(Bitmap bitmap, long window){
        Log.d(TAG, "getWindowIconFromManager: bitmap:" + bitmap + ", window:" + window + "");
        Context context = contextReference.get();
        if(context == null){
            Log.d(TAG, "context  == null ");
            return;
        }
        Log.d(TAG, "post getWindowIconFromManager: bitmap:" + bitmap.getWidth() + "x" + bitmap.getHeight() + ", window:" + window + "");
        String targetPackage = "com.termux.x11";
        Intent intent = new Intent("UPDATE_ICON");
        intent.setPackage(targetPackage);
        intent.putExtra("window_id", window);
        intent.putExtra("window_icon", bitmap);
        context.sendStickyBroadcast(intent);
        String path = "/sdcard/Download/bitmap.png";
        Log.d(TAG, "getWindowIconFromManager: path = " + path);
        saveBitmapToFile(bitmap, path);


    }

    public static void saveBitmapToFile(Bitmap bitmap, String filePath) {
        File file = new File(filePath);
        try (FileOutputStream out = new FileOutputStream(file)) {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
        } catch (IOException e) {
            e.printStackTrace();
        }
    }


    @Override
    public void startActivityForXMainWindow(WindowAttribute attribute, Class activityClass) {
        Log.d(TAG, "startActivityForXMainWindow: attribute:" + attribute + ", activityClass:" + activityClass + "");
        Context context = contextReference.get();
        if (context == null){
            return;
        }
        if (!WINDOW_XIDS.contains(attribute.getWindowPtr())) {
            Log.d(TAG, "startActivityForXMainWindow: attribute:" + attribute + " activityClass:" + activityClass);
            WINDOW_XIDS.add(attribute.getWindowPtr());
            ActivityOptions options = ActivityOptions.makeBasic();
            options.setLaunchBounds(new Rect((int)attribute.getOffsetX(),
                    (int)(attribute.getOffsetY()),
                    (int)(attribute.getWidth() + attribute.getOffsetX()),
                    (int)(attribute.getHeight() + DECORCATIONVIEW_HEIGHT + attribute.getOffsetY())));
            Intent intent = new Intent(context, activityClass);
            intent.putExtra("linux_window_attribute", attribute);
            intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent, options.toBundle());
        }
    }

    @Override
    public void startActivityForXWindow(WindowAttribute windowAttribute, Class activityClass) {

    }

    @Override
    public void startDialogForXWindow(WindowAttribute windowAttribute, Class activityClass) {

    }

    @Override
    public void startViewForXWindow(WindowAttribute windowAttribute, Class activityClass) {

    }

    @Override
    public void destroyXActivity(Activity activity) {

    }

    @Override
    public void destroyXDialog(Dialog dialog) {

    }

    @Override
    public void destroyXView(View view) {

    }

    private class TaskHandler extends Handler {

        public TaskHandler(@NonNull Looper looper) {
            super(looper);
        }

        @Override
        public void handleMessage(@NonNull Message msg) {
            super.handleMessage(msg);
            switch (msg.what) {
                case MSG_START_WM:
                    isConnected = connect2Server(display) > 0;
                    Log.d(TAG, "MSG_START_WM isConnected:" + isConnected);
                    break;
                default:
                    break;
            }
        }
    }

}
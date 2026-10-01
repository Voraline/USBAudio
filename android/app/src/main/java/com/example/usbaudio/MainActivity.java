package com.example.usbaudio;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.graphics.Color;
import android.view.View;

public final class MainActivity extends Activity {
    private UsbManager Manager;
    private final Handler UiHandler = new Handler(Looper.getMainLooper());
    private final Runnable StateUpdater = new Runnable() {
        @Override
        public void run() {
            if (NativeStarted && !AudioService.IsRequested()) {
                NativeStarted = false;
                TryOpenAccessory(null);
            }
            UiHandler.postDelayed(this, 500);
        }
    };
    private boolean NativeStarted;

    @Override
    protected void onCreate(Bundle State) {
        super.onCreate(State);
        Manager = (UsbManager) getSystemService(Context.USB_SERVICE);
        View EmptyView = new View(this);
        EmptyView.setBackgroundColor(Color.BLACK);
        setContentView(EmptyView);
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().setNavigationBarColor(Color.BLACK);
        HandleIntent(getIntent());
    }

    @Override
    protected void onResume() {
        super.onResume();
        TryOpenAccessory(null);
        UiHandler.removeCallbacks(StateUpdater);
        UiHandler.post(StateUpdater);
    }

    @Override
    protected void onPause() {
        UiHandler.removeCallbacks(StateUpdater);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
    }

    @Override
    protected void onNewIntent(Intent NewIntent) {
        super.onNewIntent(NewIntent);
        setIntent(NewIntent);
        HandleIntent(NewIntent);
    }

    private void HandleIntent(Intent AccessoryIntent) {
        if (AccessoryIntent == null) {
            TryOpenAccessory(null);
            return;
        }
        String Action = AccessoryIntent.getAction();
        if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(Action)) {
            if (NativeStarted) {
                AudioService.StopPlayback(this);
                NativeStarted = false;
            }
            return;
        }
        if (UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(Action)) {
            UsbAccessory AttachedAccessory = AccessoryIntent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY, UsbAccessory.class);
            TryOpenAccessory(AttachedAccessory);
        }
    }

    private void TryOpenAccessory(UsbAccessory SelectedAccessory) {
        if (AudioService.IsRequested()) {
            NativeStarted = true;
            return;
        }
        UsbAccessory Accessory = SelectedAccessory;
        if (Accessory == null) {
            UsbAccessory[] AttachedAccessories = Manager.getAccessoryList();
            if (AttachedAccessories != null && AttachedAccessories.length > 0) {
                Accessory = AttachedAccessories[0];
            }
        }
        if (Accessory == null) {
            return;
        }
        ParcelFileDescriptor AccessoryDescriptor;
        try {
            AccessoryDescriptor = Manager.openAccessory(Accessory);
        } catch (SecurityException Exception) {
            return;
        }
        if (AccessoryDescriptor == null) {
            return;
        }
        int NativeDescriptor = AccessoryDescriptor.detachFd();
        NativeStarted = AudioService.StartPlayback(this, NativeDescriptor);
    }
}

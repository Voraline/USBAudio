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
import android.view.Gravity;
import android.widget.TextView;

public final class MainActivity extends Activity {
    private UsbManager Manager;
    private TextView StatusView;
    private final Handler UiHandler = new Handler(Looper.getMainLooper());
    private final Runnable StateUpdater = new Runnable() {
        @Override
        public void run() {
            if (NativeStarted && !AudioService.IsRequested()) {
                NativeStarted = false;
                SetStatus("USB connection ended. Reconnect the phone.");
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
        StatusView = new TextView(this);
        StatusView.setGravity(Gravity.CENTER);
        StatusView.setTextSize(20.0f);
        StatusView.setPadding(32, 32, 32, 32);
        setContentView(StatusView);
        SetStatus("Connect this phone to the laptop by USB.");
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
            SetStatus("USB disconnected. Reconnect the phone.");
            return;
        }
        if (UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(Action)) {
            UsbAccessory AttachedAccessory;
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                AttachedAccessory = AccessoryIntent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY, UsbAccessory.class);
            } else {
                AttachedAccessory = AccessoryIntent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY);
            }
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
            SetStatus("Connect this phone to the laptop by USB.");
            return;
        }
        ParcelFileDescriptor AccessoryDescriptor;
        try {
            AccessoryDescriptor = Manager.openAccessory(Accessory);
        } catch (SecurityException Exception) {
            SetStatus("Allow USB accessory access, then reconnect the phone.");
            return;
        }
        if (AccessoryDescriptor == null) {
            SetStatus("Waiting for USB accessory permission.");
            return;
        }
        SetStatus("Starting USB audio playback...");
        int NativeDescriptor = AccessoryDescriptor.detachFd();
        NativeStarted = AudioService.StartPlayback(this, NativeDescriptor);
        SetStatus(NativeStarted ? "USB audio connected. System audio is playing." : "Could not start audio playback. Reconnect the phone.");
    }

    private void SetStatus(String Message) {
        if (StatusView != null) {
            StatusView.setText(Message);
        }
    }
}

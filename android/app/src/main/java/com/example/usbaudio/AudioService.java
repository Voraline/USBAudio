package com.example.usbaudio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ParcelFileDescriptor;

public final class AudioService extends Service {
    private static final String ChannelId = "usb_audio_playback";
    private static final String DescriptorKey = "accessory_descriptor";
    private static final int NotificationId = 1;
    private static volatile boolean Requested;
    private final Handler Watchdog = new Handler(Looper.getMainLooper());
    private int InactiveChecks;
    private final Runnable WatchdogTask = new Runnable() {
        @Override
        public void run() {
            if (!IsRunningNative()) {
                InactiveChecks++;
                if (InactiveChecks >= 5) {
                    Requested = false;
                    stopSelf();
                    return;
                }
            } else {
                InactiveChecks = 0;
            }
            Watchdog.postDelayed(this, 1000);
        }
    };

    static {
        System.loadLibrary("usb_audio_native");
    }

    private static native boolean StartNative(int Descriptor);
    private static native void StopNative();
    private static native boolean IsRunningNative();

    static boolean StartPlayback(Context ContextValue, int Descriptor) {
        Requested = true;
        Intent ServiceIntent = new Intent(ContextValue, AudioService.class);
        ServiceIntent.putExtra(DescriptorKey, Descriptor);
        try {
            if (Build.VERSION.SDK_INT >= 26) {
                ContextValue.startForegroundService(ServiceIntent);
            } else {
                ContextValue.startService(ServiceIntent);
            }
            return true;
        } catch (RuntimeException Exception) {
            Requested = false;
            try {
                ParcelFileDescriptor.adoptFd(Descriptor).close();
            } catch (Exception CloseException) {
                Requested = false;
            }
            return false;
        }
    }

    static boolean IsRequested() {
        return Requested;
    }

    static void StopPlayback(Context ContextValue) {
        Requested = false;
        StopNative();
        ContextValue.stopService(new Intent(ContextValue, AudioService.class));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        CreateNotificationChannel();
        Notification PlaybackNotification = CreateNotification();
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NotificationId, PlaybackNotification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        } else {
            startForeground(NotificationId, PlaybackNotification);
        }
        Watchdog.postDelayed(WatchdogTask, 1000);
    }

    @Override
    public int onStartCommand(Intent ServiceIntent, int Flags, int StartId) {
        int Descriptor = ServiceIntent == null ? -1 : ServiceIntent.getIntExtra(DescriptorKey, -1);
        if (Descriptor < 0 || !StartNative(Descriptor)) {
            Requested = false;
            stopSelf(StartId);
            return START_NOT_STICKY;
        }
        Requested = true;
        InactiveChecks = 0;
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        Watchdog.removeCallbacks(WatchdogTask);
        Requested = false;
        StopNative();
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent ServiceIntent) {
        return null;
    }

    private void CreateNotificationChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel Channel = new NotificationChannel(ChannelId, "USB audio playback", NotificationManager.IMPORTANCE_LOW);
            NotificationManager NotificationService = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            NotificationService.createNotificationChannel(Channel);
        }
    }

    private Notification CreateNotification() {
        Intent ActivityIntent = new Intent(this, MainActivity.class);
        PendingIntent ActivityPendingIntent = PendingIntent.getActivity(this, 0, ActivityIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder Builder = Build.VERSION.SDK_INT >= 26
            ? new Notification.Builder(this, ChannelId)
            : new Notification.Builder(this);
        return Builder.setContentTitle("USB Audio")
            .setContentText("Playing computer audio over USB")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(ActivityPendingIntent)
            .setOngoing(true)
            .build();
    }
}

/*
 * Copyright 2026 Marek Wesołowski - wesmar
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.wsanext.registration;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.drawable.Icon;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.Uri;
import android.util.Log;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * The one notification: "register this device with Google Play", with the device's Google
 * Services Framework ID. Shown once Play services have the ID, again after every start until
 * the user presses "Done" for that ID (a new installation gets a new ID and a new notification).
 */
final class Registration {
    static final String ACTION_OPEN = "org.wsanext.registration.OPEN";
    static final String ACTION_COPY = "org.wsanext.registration.COPY";
    static final String ACTION_DONE = "org.wsanext.registration.DONE";
    static final String EXTRA_ID = "id";
    static final String PAGE = "https://www.google.com/android/uncertified";

    private static final String CHANNEL = "registration";
    private static final int NOTIFICATION = 1;
    private static final Uri GSERVICES = Uri.parse("content://com.google.android.gsf.gservices");
    private static final String PREFS = "registration";
    private static final String KEY_DONE = "done_for";
    private static final String TAG = "WsaNextRegistration";
    private static final String ELEVATE_SOCKET = "/dev/socket/wsa_elevate";

    private Registration() {}

    /** The ID as Google's page asks for it (decimal), or null before Play services checked in. */
    static String gsfId(Context context) {
        final String fromProvider = gsfIdFromProvider(context);
        return fromProvider != null ? fromProvider : gsfIdFromSystem();
    }

    // Play services before 26.x answer every app holding READ_GSERVICES; current ones only
    // Google-signed apps (they log "is not approved to access the device ID" and leave it out).
    private static String gsfIdFromProvider(Context context) {
        try (Cursor cursor = context.getContentResolver().query(GSERVICES, null, null, new String[] {"android_id"}, null)) {
            if (cursor == null || !cursor.moveToFirst() || cursor.getColumnCount() < 2) return null;
            final String value = cursor.getString(1);
            if (value == null || Long.parseLong(value) == 0) return null;
            return value;
        } catch (RuntimeException e) {  // no Play services, or not a number yet
            return null;
        }
    }

    // WSA-Next's wsa-elevated hands the ID to this app only (elevate/protocol.h: "GFID").
    private static String gsfIdFromSystem() {
        try (LocalSocket socket = new LocalSocket()) {
            socket.connect(new LocalSocketAddress(ELEVATE_SOCKET, LocalSocketAddress.Namespace.FILESYSTEM));
            final OutputStream out = socket.getOutputStream();
            out.write(new byte[] {'G', 'F', 'I', 'D'});
            out.flush();
            final DataInputStream in = new DataInputStream(socket.getInputStream());
            if (in.readUnsignedByte() != 'G') return null;  // refused, or not checked in yet
            final byte[] id = new byte[in.readUnsignedByte()];
            in.readFully(id);
            final String text = new String(id, StandardCharsets.US_ASCII);
            return Long.parseLong(text) == 0 ? null : text;
        } catch (IOException | RuntimeException e) {  // no wsa-elevated (not a debuggable build)
            Log.w(TAG, "No device ID from wsa-elevated: " + e);
            return null;
        }
    }

    // A click on this app's notification does not come back to Android on WSA (it has no
    // launcher activity, so Windows cannot hand the click over). WSA-Next on Windows shows the
    // notice itself: wsa-elevated reads the ID and hands it to the host (elevate/protocol.h,
    // "RGWN"), whose notification copies it and opens the page in the Windows browser. The
    // answer: 'S' shown there, 'O' registered from Windows before, anything else - not shown.
    private static int offerOnWindows() {
        try (LocalSocket socket = new LocalSocket()) {
            socket.connect(new LocalSocketAddress(ELEVATE_SOCKET, LocalSocketAddress.Namespace.FILESYSTEM));
            final OutputStream out = socket.getOutputStream();
            out.write(new byte[] {'R', 'G', 'W', 'N'});
            out.flush();
            return new DataInputStream(socket.getInputStream()).readUnsignedByte();
        } catch (IOException | RuntimeException e) {  // no wsa-elevated (not a debuggable build)
            Log.w(TAG, "No notice on Windows: " + e);
            return -1;
        }
    }

    /**
     * Hands the ID to WSA-Next on Windows after every start and check-in, registered or not: the
     * host decides whether to notify (only for an ID not registered from Windows before) and
     * keeps "Register with Google Play" in its notification-area menu either way, so the ID can
     * be copied and the page opened again later - Google may ask for it after an update. Android's
     * own notification appears only without a host, and not again once marked as done.
     */
    static void update(Context context) {
        final String id = gsfId(context);
        if (id == null) return;  // CHECKIN_COMPLETE brings us back once there is one
        final boolean done = id.equals(prefs(context).getString(KEY_DONE, null));
        switch (offerOnWindows()) {
            case 'S':  // Windows shows it
                context.getSystemService(NotificationManager.class).cancel(NOTIFICATION);
                break;
            case 'O':  // registered from Windows before
                if (!done) done(context, id);
                break;
            default:  // no WSA-Next host, or no icon there: Android's own notification
                if (!done) show(context, id);
                break;
        }
    }

    static void done(Context context, String id) {
        prefs(context).edit().putString(KEY_DONE, id).apply();
        context.getSystemService(NotificationManager.class).cancel(NOTIFICATION);
    }

    private static SharedPreferences prefs(Context context) {
        return context.createDeviceProtectedStorageContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static void show(Context context, String id) {
        final NotificationManager manager = context.getSystemService(NotificationManager.class);
        manager.createNotificationChannel(
                new NotificationChannel(CHANNEL, context.getString(R.string.channel), NotificationManager.IMPORTANCE_DEFAULT));
        final PendingIntent open = button(context, ACTION_OPEN, id, 0);
        final Notification notification = new Notification.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle(context.getString(R.string.title))
                .setContentText(context.getString(R.string.text, id))
                .setStyle(new Notification.BigTextStyle().bigText(context.getString(R.string.text_long, id)))
                .setContentIntent(open)
                .setOnlyAlertOnce(true)
                .addAction(action(context, R.string.copy, button(context, ACTION_COPY, id, 1)))
                .addAction(action(context, R.string.open, open))
                .addAction(action(context, R.string.done, button(context, ACTION_DONE, id, 2)))
                .build();
        manager.notify(NOTIFICATION, notification);
    }

    private static PendingIntent button(Context context, String action, String id, int request) {
        final Intent intent = new Intent(action).setClass(context, ActionReceiver.class).putExtra(EXTRA_ID, id);
        return PendingIntent.getBroadcast(context, request, intent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private static Notification.Action action(Context context, int label, PendingIntent intent) {
        return new Notification.Action.Builder(Icon.createWithResource(context, android.R.drawable.ic_menu_info_details),
                context.getString(label), intent).build();
    }
}

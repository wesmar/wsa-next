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

import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;

/** The notification's buttons (where Android's notification is shown): copy the ID, open the page, or done. */
public final class ActionReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        final String id = intent.getStringExtra(Registration.EXTRA_ID);
        if (id == null) return;
        final String action = intent.getAction();
        if (Registration.ACTION_COPY.equals(action)) {
            context.getSystemService(ClipboardManager.class).setPrimaryClip(ClipData.newPlainText(context.getString(R.string.clip_label), id));
            Toast.makeText(context, R.string.copied, Toast.LENGTH_SHORT).show();
        } else if (Registration.ACTION_OPEN.equals(action)) {
            context.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(Registration.PAGE)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } else if (Registration.ACTION_DONE.equals(action)) {
            Registration.done(context, id);
        }
    }
}

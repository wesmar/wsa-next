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
import android.content.Context;
import android.content.Intent;

/**
 * Android has started, or Play services have checked in with Google (which is when a new
 * installation gets its ID). Anyone may send these; the receiver only looks the ID up again.
 */
public final class TriggerReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        final PendingResult pending = goAsync();  // the provider query must not hold the main thread
        new Thread(() -> {
            try {
                Registration.update(context);
            } finally {
                pending.finish();
            }
        }, "WsaNextRegistration").start();
    }
}

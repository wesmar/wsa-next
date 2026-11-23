/*
 * Copyright 2022 The Android Open Source Project
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

#define LOG_TAG "android.hardware.bluetooth.service.wsa"

#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "BluetoothHci.h"
#include "log/log.h"

using ::aidl::android::hardware::bluetooth::impl::BluetoothHci;

int main(int /* argc */, char** /* argv */) {
  ALOGI("Bluetooth HAL starting");
  if (!ABinderProcess_setThreadPoolMaxThreadCount(0)) {
    ALOGE("failed to set thread pool max thread count");
    return 1;
  }
  std::shared_ptr<BluetoothHci> service = ndk::SharedRefBase::make<BluetoothHci>();
  const std::string instance = std::string() + BluetoothHci::descriptor + "/default";
  if (AServiceManager_addService(service->asBinder().get(), instance.c_str()) != STATUS_OK) {
    ALOGE("Could not register as a service!");
    return 1;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}

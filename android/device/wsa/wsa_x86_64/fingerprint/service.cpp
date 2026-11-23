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
#define LOG_TAG "android.hardware.biometrics.fingerprint.wsa"

#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "WsaFingerprint.h"
#include "log/log.h"

using ::aidl::android::hardware::biometrics::fingerprint::wsa::WsaFingerprint;

int main() {
  ALOGI("WSA Windows Hello Fingerprint HAL starting");
  ABinderProcess_setThreadPoolMaxThreadCount(0);
  auto service = ndk::SharedRefBase::make<WsaFingerprint>();
  const std::string instance = std::string() + WsaFingerprint::descriptor + "/default";
  if (AServiceManager_addService(service->asBinder().get(), instance.c_str()) != STATUS_OK) {
    ALOGE("Could not register %s", instance.c_str());
    return 1;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}

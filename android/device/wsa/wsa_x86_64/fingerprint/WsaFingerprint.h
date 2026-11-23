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

// Fingerprint HAL of WSA: Windows Hello is the sensor.
//
// Android sees one fingerprint sensor of WEAK strength (the verification happens in Windows, not
// in a TEE that could mint hardware auth tokens for KeyMint) with at most one template per user,
// "Windows Hello". Enrolling links Android to the Windows Hello of the signed-in Windows user; it
// needs no finger and no prompt, because every authentication is verified by Windows Hello itself
// (fingerprint, face or PIN, in Windows' own dialog). WSA-Next's system_server service enrolls it
// automatically, so no Android screen lock is needed (a lock screen would hide WSA's windows).
//
// The Windows side is the Windows Hello bridge of WSA-Next.exe, which connects to this HAL over a
// Hyper-V socket (vsock port "WSFP") when it starts and keeps the connection. It pushes whether
// Windows Hello is available whenever that changes (published in sys.wsa.fingerprint.hello) and
// answers verification requests; nothing here polls. Enrollments and authenticator IDs are kept
// in persist.wsa.fingerprint.u<user>, so the framework finds them again after a restart.

#pragma once

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::biometrics::fingerprint::wsa {

namespace common = ::aidl::android::hardware::biometrics::common;
namespace keymaster = ::aidl::android::hardware::keymaster;

// What the HAL remembers per Android user.
struct UserState {
  int64_t authenticatorId = 0;
  std::vector<int32_t> enrollments;
};

class WsaSession : public BnSession {
 public:
  WsaSession(int32_t sensorId, int32_t userId, std::shared_ptr<ISessionCallback> cb);
  ~WsaSession() override;

  ndk::ScopedAStatus generateChallenge() override;
  ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
  ndk::ScopedAStatus enroll(const keymaster::HardwareAuthToken& hat,
                            std::shared_ptr<common::ICancellationSignal>* out) override;
  ndk::ScopedAStatus authenticate(int64_t operationId,
                                  std::shared_ptr<common::ICancellationSignal>* out) override;
  ndk::ScopedAStatus detectInteraction(
      std::shared_ptr<common::ICancellationSignal>* out) override;
  ndk::ScopedAStatus enumerateEnrollments() override;
  ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& enrollmentIds) override;
  ndk::ScopedAStatus getAuthenticatorId() override;
  ndk::ScopedAStatus invalidateAuthenticatorId() override;
  ndk::ScopedAStatus resetLockout(const keymaster::HardwareAuthToken& hat) override;
  ndk::ScopedAStatus close() override;

  ndk::ScopedAStatus onPointerDown(int32_t pointerId, int32_t x, int32_t y, float minor,
                                   float major) override;
  ndk::ScopedAStatus onPointerUp(int32_t pointerId) override;
  ndk::ScopedAStatus onUiReady() override;

  ndk::ScopedAStatus authenticateWithContext(
      int64_t operationId, const common::OperationContext& context,
      std::shared_ptr<common::ICancellationSignal>* out) override;
  ndk::ScopedAStatus enrollWithContext(
      const keymaster::HardwareAuthToken& hat, const common::OperationContext& context,
      std::shared_ptr<common::ICancellationSignal>* out) override;
  ndk::ScopedAStatus detectInteractionWithContext(
      const common::OperationContext& context,
      std::shared_ptr<common::ICancellationSignal>* out) override;
  ndk::ScopedAStatus onPointerDownWithContext(const PointerContext& context) override;
  ndk::ScopedAStatus onPointerUpWithContext(const PointerContext& context) override;
  ndk::ScopedAStatus onContextChanged(const common::OperationContext& context) override;
  ndk::ScopedAStatus onPointerCancelWithContext(const PointerContext& context) override;
  ndk::ScopedAStatus setIgnoreDisplayTouches(bool shouldIgnore) override;

 private:
  // One operation at a time. A new operation, close() and the destructor first cancel one that is
  // still running (the Windows Hello dialog closes) and then wait for it: the binder thread, the
  // only one, never waits for a person.
  void StartWorker(std::function<void(const std::shared_ptr<std::atomic<bool>>& canceled)> work,
                   std::shared_ptr<common::ICancellationSignal>* out);
  void JoinWorker();
  void StartAuthenticate(int64_t operationId, bool waitForLockScreen,
                         std::shared_ptr<common::ICancellationSignal>* out);
  void Enroll();
  void Authenticate(int64_t operationId, const std::shared_ptr<std::atomic<bool>>& canceled);
  void Save();  // mState to persist.wsa.fingerprint.u<user> (call with mLock held)

  const int32_t mUserId;
  const std::shared_ptr<ISessionCallback> mCb;

  std::mutex mLock;
  UserState mState;
  std::thread mWorker;
  std::shared_ptr<std::atomic<bool>> mWorkerCanceled;  // the running operation's cancellation flag
  // The running authentication waits for the lock screen to be shown (see WaitsForLockScreen).
  std::atomic<bool> mWaitForLockScreen{false};
};

class WsaFingerprint : public BnFingerprint {
 public:
  WsaFingerprint();

  ndk::ScopedAStatus getSensorProps(std::vector<SensorProps>* out) override;
  ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                   const std::shared_ptr<ISessionCallback>& cb,
                                   std::shared_ptr<ISession>* out) override;

};

}  // namespace aidl::android::hardware::biometrics::fingerprint::wsa

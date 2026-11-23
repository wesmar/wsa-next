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

#include "WsaFingerprint.h"

#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>  // before linux/vm_sockets.h, which needs struct sockaddr
#include <unistd.h>

#include <linux/vm_sockets.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <random>

#include "log/log.h"

namespace aidl::android::hardware::biometrics::fingerprint::wsa {
namespace {

// Wire protocol of src/biometrics/Protocol.h on the Windows host: an 8-byte header, then an
// optional UTF-8 payload (the prompt of a request).
constexpr uint32_t kBridgePort = 0x57534650;  // "WSFP"
constexpr uint32_t kMagic = 0x50465357;       // 'W','S','F','P' little-endian
constexpr uint8_t kOpAuthenticate = 3;        // HAL -> bridge request, bridge -> HAL answer
constexpr uint8_t kOpAvailability = 4;        // bridge -> HAL, whenever it changes
constexpr uint8_t kOpCancel = 5;              // HAL -> bridge: close the Windows Hello dialog
constexpr uint8_t kStatusOk = 0;
constexpr uint8_t kStatusVerified = 1;
constexpr uint8_t kStatusCanceled = 2;
constexpr uint8_t kStatusNotAvailable = 3;
constexpr uint8_t kStatusBusy = 4;
constexpr uint8_t kNoBridge = 0xFF;  // local: no bridge connected, or it went away

struct FrameHeader {
  uint32_t magic = kMagic;
  uint8_t op = 0;
  uint8_t status = 0;
  uint16_t payloadLen = 0;
};
static_assert(sizeof(FrameHeader) == 8);

constexpr char kAvailableProperty[] = "sys.wsa.fingerprint.hello";
constexpr char kStatePrefix[] = "persist.wsa.fingerprint.u";
constexpr int32_t kTemplateId = 1;  // the one template: "Windows Hello"

int64_t RandomId() {
  std::random_device rd;
  const uint64_t v = (static_cast<uint64_t>(rd()) << 32) | static_cast<uint64_t>(rd());
  return static_cast<int64_t>(v == 0 ? 1 : v);
}

bool SendAll(int fd, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t n = TEMP_FAILURE_RETRY(send(fd, p + done, size - done, MSG_NOSIGNAL));
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

bool RecvAll(int fd, void* data, size_t size) {
  auto* p = static_cast<uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t n = TEMP_FAILURE_RETRY(recv(fd, p + done, size - done, 0));
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

// The connection from the Windows Hello bridge. One thread accepts it and reads what the bridge
// sends (availability pushes, verification answers); requests are written from the session
// threads. All waits are on file descriptors or a condition variable, without timeouts.
class HelloLink {
 public:
  static HelloLink& Get() {
    static HelloLink* link = new HelloLink();  // lives as long as the service
    return *link;
  }

  bool Available() {
    std::lock_guard<std::mutex> guard(mLock);
    return mAvailable;
  }

  // Asks Windows Hello. Returns its answer, kStatusCanceled when `canceled` was set (the dialog is
  // closed), or kNoBridge when the bridge is not there or goes away meanwhile.
  uint8_t Authenticate(const std::string& prompt, const std::shared_ptr<std::atomic<bool>>& canceled) {
    std::unique_lock<std::mutex> lock(mLock);
    if (mConnection < 0) return kNoBridge;
    const uint16_t length = static_cast<uint16_t>(std::min<size_t>(prompt.size(), 512));
    const FrameHeader request{kMagic, kOpAuthenticate, 0, length};
    if (!SendAll(mConnection, &request, sizeof request) || !SendAll(mConnection, prompt.data(), length)) return kNoBridge;
    const uint64_t generation = mGeneration;
    mAnswer.reset();
    mWaiting = true;
    bool cancelSent = false;
    for (;;) {
      mChanged.wait(lock, [&] { return mAnswer.has_value() || mGeneration != generation || (canceled->load() && !cancelSent); });
      if (mAnswer.has_value()) break;
      if (mGeneration != generation) {
        mWaiting = false;
        return kNoBridge;
      }
      // Canceled: ask Windows to close the dialog; its answer ("canceled") ends the wait.
      const FrameHeader cancel{kMagic, kOpCancel, 0, 0};
      cancelSent = true;
      if (!SendAll(mConnection, &cancel, sizeof cancel)) {
        mWaiting = false;
        return kStatusCanceled;
      }
    }
    mWaiting = false;
    return canceled->load() ? kStatusCanceled : *mAnswer;
  }

  // Waits while `waiting` holds, without asking Windows. True when it stopped holding (ask now),
  // false when the operation was canceled meanwhile.
  bool WaitWhile(const std::shared_ptr<std::atomic<bool>>& canceled, const std::atomic<bool>& waiting) {
    std::unique_lock<std::mutex> lock(mLock);
    mChanged.wait(lock, [&] { return canceled->load() || !waiting.load(); });
    return !canceled->load();
  }

  // Wakes a waiting Authenticate or WaitWhile (the cancellation signal was set, or the context
  // changed). Taking the lock first orders this after the waiter's predicate check: either it sees
  // the change or it is already waiting.
  void Notify() {
    { std::lock_guard<std::mutex> guard(mLock); }
    mChanged.notify_all();
  }

 private:
  HelloLink() {
    ::android::base::SetProperty(kAvailableProperty, "0");
    std::thread(&HelloLink::Serve, this).detach();
  }

  void SetAvailableLocked(bool available) {
    if (available == mAvailable) return;
    mAvailable = available;
    ::android::base::SetProperty(kAvailableProperty, available ? "1" : "0");
    ALOGI("Windows Hello %s", available ? "available" : "not available");
  }

  void DropLocked() {
    if (mConnection >= 0) ::close(mConnection);
    mConnection = -1;
    ++mGeneration;  // fails a waiting request
    SetAvailableLocked(false);
    mChanged.notify_all();
  }

  void Serve() {
    const int listener = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = kBridgePort;
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listener, 1) != 0) {
      ALOGE("Cannot listen on vsock port 0x%08x: %s", kBridgePort, strerror(errno));
      return;
    }
    for (;;) {
      int connection = -1;
      {
        std::lock_guard<std::mutex> guard(mLock);
        connection = mConnection;
      }
      pollfd fds[2] = {};
      fds[0].fd = listener;
      fds[0].events = POLLIN;
      fds[1].fd = connection;  // -1: ignored by poll
      fds[1].events = POLLIN | POLLRDHUP;
      if (TEMP_FAILURE_RETRY(poll(fds, 2, -1)) < 0) continue;

      if (fds[0].revents & POLLIN) {
        const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) {
          std::lock_guard<std::mutex> guard(mLock);
          DropLocked();  // a new bridge replaces the previous connection
          mConnection = fd;
          ALOGI("The Windows Hello bridge connected");
        }
        continue;
      }
      if (connection < 0 || fds[1].revents == 0) continue;
      FrameHeader frame{};
      // Data first: a last frame may come with the hangup (read until recv says the end).
      bool ok = (fds[1].revents & POLLIN) && RecvAll(connection, &frame, sizeof frame) && frame.magic == kMagic;
      for (uint16_t left = ok ? frame.payloadLen : 0; ok && left > 0;) {  // no payload is expected: skip it
        uint8_t skip[64];
        const size_t n = std::min<size_t>(left, sizeof skip);
        ok = RecvAll(connection, skip, n);
        left = static_cast<uint16_t>(left - n);
      }
      std::lock_guard<std::mutex> guard(mLock);
      if (connection != mConnection) continue;
      if (!ok) {
        ALOGI("The Windows Hello bridge disconnected");
        DropLocked();
      } else if (frame.op == kOpAvailability) {
        SetAvailableLocked(frame.status == kStatusOk);
      } else if (frame.op == kOpAuthenticate && mWaiting) {
        mAnswer = frame.status;
        mChanged.notify_all();
      }
    }
  }

  std::mutex mLock;
  std::condition_variable mChanged;
  int mConnection = -1;
  uint64_t mGeneration = 0;  // advanced whenever the connection ends
  bool mAvailable = false;
  bool mWaiting = false;
  std::optional<uint8_t> mAnswer;
};

class WsaCancelSignal : public common::BnCancellationSignal {
 public:
  explicit WsaCancelSignal(std::shared_ptr<std::atomic<bool>> flag) : mFlag(std::move(flag)) {}
  ndk::ScopedAStatus cancel() override {
    mFlag->store(true);
    HelloLink::Get().Notify();
    return ndk::ScopedAStatus::ok();
  }

 private:
  std::shared_ptr<std::atomic<bool>> mFlag;
};

// The lock screen listens for a finger also while it is not shown: the screen is off or dozing
// (display state NO_UI, AOD, SCREENSAVER) or it is not known yet, so that a touch of the sensor
// wakes and unlocks a phone. A phone's sensor then waits silently for a finger; opening Windows
// Hello for it asked the person every time Android went to sleep (each 15 seconds without input,
// and whenever the last Android window closed). So the HAL waits too, and asks Windows only once
// the lock screen is shown. Apps (BiometricPrompt) are asked at once, as before.
bool WaitsForLockScreen(const common::OperationContext& context) {
  return context.reason == common::OperationReason::KEYGUARD && context.displayState != common::DisplayState::LOCKSCREEN;
}

std::string StateProperty(int32_t userId) { return kStatePrefix + std::to_string(userId); }

// "<authenticator id>:<template>,<template>..."
UserState LoadState(int32_t userId) {
  UserState state;
  const std::string value = ::android::base::GetProperty(StateProperty(userId), "");
  const std::vector<std::string> parts = ::android::base::Split(value, ":");
  if (parts.size() == 2) {
    ::android::base::ParseInt(parts[0], &state.authenticatorId);
    for (const std::string& id : ::android::base::Split(parts[1], ",")) {
      int32_t template_id = 0;
      if (::android::base::ParseInt(id, &template_id) && template_id > 0) state.enrollments.push_back(template_id);
    }
  }
  if (state.authenticatorId == 0) state.authenticatorId = RandomId();
  return state;
}

}  // namespace

WsaSession::WsaSession(int32_t /*sensorId: there is only one*/, int32_t userId, std::shared_ptr<ISessionCallback> cb)
    : mUserId(userId), mCb(std::move(cb)), mState(LoadState(userId)) {}

WsaSession::~WsaSession() { JoinWorker(); }

void WsaSession::JoinWorker() {
  if (!mWorker.joinable()) return;
  // Harmless when the operation has ended already; ends the wait for Windows when it has not.
  if (mWorkerCanceled && !mWorkerCanceled->exchange(true)) HelloLink::Get().Notify();
  mWorker.join();
  mWorkerCanceled.reset();
}

void WsaSession::StartWorker(std::function<void(const std::shared_ptr<std::atomic<bool>>&)> work,
                             std::shared_ptr<common::ICancellationSignal>* out) {
  JoinWorker();
  auto canceled = std::make_shared<std::atomic<bool>>(false);
  *out = ndk::SharedRefBase::make<WsaCancelSignal>(canceled);
  mWorkerCanceled = canceled;
  mWorker = std::thread([work = std::move(work), canceled] { work(canceled); });
}

void WsaSession::Save() {
  std::string ids;
  for (const int32_t id : mState.enrollments) ids += (ids.empty() ? "" : ",") + std::to_string(id);
  if (!::android::base::SetProperty(StateProperty(mUserId), std::to_string(mState.authenticatorId) + ":" + ids)) {
    ALOGE("Could not save the fingerprint state of user %d", mUserId);
  }
}

ndk::ScopedAStatus WsaSession::generateChallenge() {
  mCb->onChallengeGenerated(RandomId());
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::revokeChallenge(int64_t challenge) {
  mCb->onChallengeRevoked(challenge);
  return ndk::ScopedAStatus::ok();
}

// Linking needs Windows Hello to be there, not a verification (every use is verified).
void WsaSession::Enroll() {
  if (!HelloLink::Get().Available()) {
    ALOGW("enroll: Windows Hello is not available");
    mCb->onError(Error::HW_UNAVAILABLE, 0);
    return;
  }
  {
    std::lock_guard<std::mutex> guard(mLock);
    if (std::find(mState.enrollments.begin(), mState.enrollments.end(), kTemplateId) == mState.enrollments.end()) {
      mState.enrollments.push_back(kTemplateId);
    }
    Save();
  }
  ALOGI("enrolled: Windows Hello linked for user %d", mUserId);
  mCb->onEnrollmentProgress(kTemplateId, 0);
}

void WsaSession::Authenticate(int64_t operationId, const std::shared_ptr<std::atomic<bool>>& canceled) {
  int64_t authenticatorId = 0;
  {
    std::lock_guard<std::mutex> guard(mLock);
    if (mState.enrollments.empty()) {
      mCb->onError(Error::UNABLE_TO_PROCESS, 0);
      return;
    }
    authenticatorId = mState.authenticatorId;
  }
  if (mWaitForLockScreen.load()) {
    ALOGI("authenticate: the lock screen asks while it is not shown, waiting like a sensor");
    if (!HelloLink::Get().WaitWhile(canceled, mWaitForLockScreen)) {
      mCb->onError(Error::CANCELED, 0);
      return;
    }
    ALOGI("authenticate: the lock screen is shown, asking Windows Hello");
  }
  const uint8_t status = HelloLink::Get().Authenticate("Android in WSA-Next asks you to verify your identity", canceled);
  if (status == kStatusCanceled) {
    mCb->onError(Error::CANCELED, 0);
    return;
  }
  if (status == kNoBridge || status == kStatusNotAvailable || status == kStatusBusy) {
    mCb->onError(Error::HW_UNAVAILABLE, 0);
    return;
  }
  if (status != kStatusVerified) {
    mCb->onAuthenticationFailed();
    return;
  }
  // No TEE can sign this token: the sensor is WEAK, so the framework never hands it to KeyMint.
  keymaster::HardwareAuthToken hat;
  hat.challenge = operationId;
  hat.userId = mUserId;
  hat.authenticatorId = authenticatorId;
  hat.authenticatorType = keymaster::HardwareAuthenticatorType::FINGERPRINT;
  hat.timestamp.milliSeconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  mCb->onAuthenticationSucceeded(kTemplateId, hat);
}

ndk::ScopedAStatus WsaSession::enroll(const keymaster::HardwareAuthToken& /*hat*/,
                                      std::shared_ptr<common::ICancellationSignal>* out) {
  StartWorker([this](const std::shared_ptr<std::atomic<bool>>&) { Enroll(); }, out);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::authenticate(int64_t operationId,
                                            std::shared_ptr<common::ICancellationSignal>* out) {
  StartAuthenticate(operationId, false, out);
  return ndk::ScopedAStatus::ok();
}

void WsaSession::StartAuthenticate(int64_t operationId, bool waitForLockScreen,
                                   std::shared_ptr<common::ICancellationSignal>* out) {
  JoinWorker();  // the previous operation ends first: the flag below belongs to the new one only
  mWaitForLockScreen = waitForLockScreen;
  StartWorker([this, operationId](const std::shared_ptr<std::atomic<bool>>& canceled) { Authenticate(operationId, canceled); }, out);
}

ndk::ScopedAStatus WsaSession::detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) {
  *out = ndk::SharedRefBase::make<WsaCancelSignal>(std::make_shared<std::atomic<bool>>(false));
  mCb->onError(Error::UNABLE_TO_PROCESS, 0);  // not supported (supportsDetectInteraction = false)
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::enumerateEnrollments() {
  std::vector<int32_t> ids;
  {
    std::lock_guard<std::mutex> guard(mLock);
    ids = mState.enrollments;
  }
  mCb->onEnrollmentsEnumerated(ids);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
  {
    std::lock_guard<std::mutex> guard(mLock);
    for (const int32_t id : enrollmentIds) {
      mState.enrollments.erase(std::remove(mState.enrollments.begin(), mState.enrollments.end(), id), mState.enrollments.end());
    }
    Save();
  }
  mCb->onEnrollmentsRemoved(enrollmentIds);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::getAuthenticatorId() {
  int64_t id = 0;
  {
    std::lock_guard<std::mutex> guard(mLock);
    id = mState.authenticatorId;
  }
  mCb->onAuthenticatorIdRetrieved(id);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::invalidateAuthenticatorId() {
  int64_t id = 0;
  {
    std::lock_guard<std::mutex> guard(mLock);
    mState.authenticatorId = RandomId();
    id = mState.authenticatorId;
    Save();
  }
  mCb->onAuthenticatorIdInvalidated(id);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::resetLockout(const keymaster::HardwareAuthToken& /*hat*/) {
  mCb->onLockoutCleared();  // Windows Hello keeps its own lockout
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::close() {
  JoinWorker();
  mCb->onSessionClosed();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::onPointerDown(int32_t, int32_t, int32_t, float, float) { return ndk::ScopedAStatus::ok(); }
ndk::ScopedAStatus WsaSession::onPointerUp(int32_t) { return ndk::ScopedAStatus::ok(); }
ndk::ScopedAStatus WsaSession::onUiReady() { return ndk::ScopedAStatus::ok(); }

ndk::ScopedAStatus WsaSession::authenticateWithContext(int64_t operationId, const common::OperationContext& context,
                                                       std::shared_ptr<common::ICancellationSignal>* out) {
  ALOGI("authenticate: reason %d, display state %d", static_cast<int>(context.reason), static_cast<int>(context.displayState));
  StartAuthenticate(operationId, WaitsForLockScreen(context), out);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaSession::enrollWithContext(const keymaster::HardwareAuthToken& hat, const common::OperationContext&,
                                                 std::shared_ptr<common::ICancellationSignal>* out) {
  return enroll(hat, out);
}

ndk::ScopedAStatus WsaSession::detectInteractionWithContext(const common::OperationContext&,
                                                            std::shared_ptr<common::ICancellationSignal>* out) {
  return detectInteraction(out);
}

ndk::ScopedAStatus WsaSession::onPointerDownWithContext(const PointerContext&) { return ndk::ScopedAStatus::ok(); }
ndk::ScopedAStatus WsaSession::onPointerUpWithContext(const PointerContext&) { return ndk::ScopedAStatus::ok(); }
// The running operation's context changed, e.g. the lock screen came up after the screen woke.
ndk::ScopedAStatus WsaSession::onContextChanged(const common::OperationContext& context) {
  const bool wait = WaitsForLockScreen(context);
  if (mWaitForLockScreen.exchange(wait) != wait) {
    ALOGI("context changed: reason %d, display state %d", static_cast<int>(context.reason), static_cast<int>(context.displayState));
    HelloLink::Get().Notify();
  }
  return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus WsaSession::onPointerCancelWithContext(const PointerContext&) { return ndk::ScopedAStatus::ok(); }
ndk::ScopedAStatus WsaSession::setIgnoreDisplayTouches(bool) { return ndk::ScopedAStatus::ok(); }

WsaFingerprint::WsaFingerprint() { (void)HelloLink::Get(); }  // starts listening for the bridge

ndk::ScopedAStatus WsaFingerprint::getSensorProps(std::vector<SensorProps>* out) {
  const common::ComponentInfo hw{"windowsHello", "WSA-Next", "Windows Hello", "", ""};
  // WEAK: verified by Windows, not by an Android TEE (no KeyMint-bound keys).
  const common::CommonProps commonProps{1 /* sensorId */, common::SensorStrength::WEAK,
                                        1 /* maxEnrollmentsPerUser: "Windows Hello" */, {hw}};
  SensorLocation location;
  *out = {{commonProps,
           FingerprintSensorType::REAR,
           {location},
           false /* supportsNavigationGestures */,
           false /* supportsDetectInteraction */,
           false /* halHandlesDisplayTouches */,
           false /* halControlsIllumination */,
           std::nullopt /* touchDetectionParameters */}};
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus WsaFingerprint::createSession(int32_t sensorId, int32_t userId,
                                                 const std::shared_ptr<ISessionCallback>& cb,
                                                 std::shared_ptr<ISession>* out) {
  *out = ndk::SharedRefBase::make<WsaSession>(sensorId, userId, cb);
  return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::fingerprint::wsa

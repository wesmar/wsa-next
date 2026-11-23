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

#include "BluetoothHci.h"

#include <android-base/properties.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>  // before linux/vm_sockets.h, which needs struct sockaddr
#include <unistd.h>

#include <linux/vm_sockets.h>


#include "log/log.h"

using namespace ::android::hardware::bluetooth::hci;
using aidl::android::hardware::bluetooth::Status;

namespace aidl::android::hardware::bluetooth::impl {
namespace {

// Read by the WSA services in system_server (Bluetooth is offered only while it is "1").
constexpr char kBridgeProperty[] = "sys.wsa.bluetooth.bridge";

void PublishBridge(bool present) { ::android::base::SetProperty(kBridgeProperty, present ? "1" : "0"); }

}  // namespace

void OnDeath(void* cookie);

// Closes the HAL when the Bluetooth stack (the only client) dies, so that its restart can
// initialize again.
class BluetoothDeathRecipient {
 public:
  explicit BluetoothDeathRecipient(BluetoothHci* hci) : mHci(hci) {}

  void LinkToDeath(const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
    mCb = cb;
    clientDeathRecipient_ = AIBinder_DeathRecipient_new(OnDeath);
    auto status = AIBinder_linkToDeath(mCb->asBinder().get(), clientDeathRecipient_, this /* cookie */);
    LOG_ALWAYS_FATAL_IF(status != STATUS_OK, "Unable to link to death recipient");
  }

  void serviceDied() {
    if (mCb != nullptr && !AIBinder_isAlive(mCb->asBinder().get())) {
      ALOGE("Bluetooth remote service has died");
    } else {
      ALOGE("BluetoothDeathRecipient::serviceDied called but service not dead");
      return;
    }
    {
      std::lock_guard<std::mutex> guard(mHasDiedMutex);
      has_died_ = true;
    }
    mHci->close();
  }

  bool getHasDied() {
    std::lock_guard<std::mutex> guard(mHasDiedMutex);
    return has_died_;
  }

 private:
  BluetoothHci* mHci;
  std::shared_ptr<IBluetoothHciCallbacks> mCb;
  AIBinder_DeathRecipient* clientDeathRecipient_{nullptr};
  std::mutex mHasDiedMutex;
  bool has_died_{false};
};

void OnDeath(void* cookie) {
  static_cast<BluetoothDeathRecipient*>(cookie)->serviceDied();
}

BluetoothHci::BluetoothHci() {
  mDeathRecipient = std::make_shared<BluetoothDeathRecipient>(this);
  mWake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  PublishBridge(false);
  // The service lives as long as the process: this thread is never joined.
  std::thread(&BluetoothHci::serveBridge, this).detach();
}

void BluetoothHci::wake() {
  const uint64_t one = 1;
  (void)TEMP_FAILURE_RETRY(write(mWake, &one, sizeof one));
}

void BluetoothHci::dropBridgeLocked() {
  if (mFd >= 0) {
    ::close(mFd);
    mFd = -1;
    ALOGI("The Windows Bluetooth bridge disconnected");
  }
  PublishBridge(false);
  mStateChanged.notify_all();
}

void BluetoothHci::serveBridge() {
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
    pollfd fds[3] = {};
    fds[0].fd = listener;
    fds[0].events = POLLIN;
    fds[1].fd = mWake;
    fds[1].events = POLLIN;
    nfds_t count = 2;
    int idle = -1;
    {
      // While the stack uses the connection, its reader watches it; idle, this thread does.
      std::lock_guard<std::mutex> guard(mStateMutex);
      if (mFd >= 0 && mState == HalState::READY) idle = mFd;
    }
    if (idle >= 0) {
      fds[2].fd = idle;
      fds[2].events = POLLIN | POLLRDHUP;
      count = 3;
    }
    if (TEMP_FAILURE_RETRY(poll(fds, count, -1)) < 0) continue;

    if (fds[1].revents & POLLIN) {
      uint64_t value = 0;
      (void)TEMP_FAILURE_RETRY(read(mWake, &value, sizeof value));
    }
    if (fds[0].revents & POLLIN) {
      const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) {
        std::lock_guard<std::mutex> guard(mStateMutex);
        if (mState == HalState::READY) {
          if (mFd >= 0) ::close(mFd);  // a new bridge replaces a stale connection
          mFd = fd;
          mBridgeLost = false;
          ALOGI("The Windows Bluetooth bridge connected");
          PublishBridge(true);
          mStateChanged.notify_all();
        } else {
          // The stack still talks to the previous connection: switch when it lets go.
          if (mPendingFd >= 0) ::close(mPendingFd);
          mPendingFd = fd;
        }
      }
    }
    if (count == 3 && fds[2].revents) {
      // Idle: the bridge went away, or sent a leftover (the answer to the reset of close()).
      std::lock_guard<std::mutex> guard(mStateMutex);
      if (mFd == idle && mState == HalState::READY) {
        char buffer[512];
        if ((fds[2].revents & (POLLHUP | POLLRDHUP | POLLERR)) || read(idle, buffer, sizeof buffer) <= 0) dropBridgeLocked();
      }
    }
  }
}

void BluetoothHci::readerLoop(int fd) {
  pollfd fds[2] = {};
  fds[0].fd = fd;
  fds[0].events = POLLIN;
  fds[1].fd = mReaderStop;
  fds[1].events = POLLIN;
  for (;;) {
    if (TEMP_FAILURE_RETRY(poll(fds, 2, -1)) < 0) return;
    if (fds[1].revents) return;  // stopReader()
    if (fds[0].revents & POLLIN) mH4->OnDataReady();
    if (fds[0].revents & (POLLHUP | POLLERR)) {
      mH4->OnDataReady();  // lets H4Protocol see the end and report the disconnection
      return;
    }
    if (mBridgeLost) return;
  }
}

void BluetoothHci::stopReader() {
  if (mReaderStop >= 0) {
    const uint64_t one = 1;
    (void)TEMP_FAILURE_RETRY(write(mReaderStop, &one, sizeof one));
  }
  if (mReader.joinable()) mReader.join();
  if (mReaderStop >= 0) {
    ::close(mReaderStop);
    mReaderStop = -1;
  }
}

ndk::ScopedAStatus BluetoothHci::initialize(const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
  ALOGI(__func__);
  if (cb == nullptr) {
    ALOGE("cb == nullptr! -> Unable to call initializationComplete(ERR)");
    return ndk::ScopedAStatus::fromServiceSpecificError(STATUS_BAD_VALUE);
  }

  std::unique_lock<std::mutex> lock(mStateMutex);
  if (mState != HalState::READY) {
    const HalState old_state = mState;
    lock.unlock();
    ALOGE("initialize: Unexpected State %d", static_cast<int>(old_state));
    close();
    cb->initializationComplete(Status::ALREADY_INITIALIZED);
    return ndk::ScopedAStatus::ok();
  }
  // system_server offers Bluetooth only while a bridge is connected; this is the race of a
  // bridge that left just now.
  if (mFd < 0) {
    lock.unlock();
    ALOGE("No Windows Bluetooth bridge is connected (vsock port 0x%08x)", kBridgePort);
    cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE);
    return ndk::ScopedAStatus::ok();
  }
  mState = HalState::INITIALIZING;
  const int fd = mFd;
  lock.unlock();

  mCb = cb;
  mDeathRecipient->LinkToDeath(mCb);
  mH4 = std::make_shared<H4Protocol>(
      fd,
      [](const std::vector<uint8_t>& /* raw_command */) {
        ALOGE("The bridge sent a command packet; dropped");
      },
      [this](const std::vector<uint8_t>& raw_acl) { mCb->aclDataReceived(raw_acl); },
      [this](const std::vector<uint8_t>& raw_sco) { mCb->scoDataReceived(raw_sco); },
      [this](const std::vector<uint8_t>& raw_event) { mCb->hciEventReceived(raw_event); },
      [this](const std::vector<uint8_t>& raw_iso) { mCb->isoDataReceived(raw_iso); },
      [this]() {
        // The stack notices the missing controller through its command timeout; the published
        // absence makes system_server turn Bluetooth off, and close() releases the connection.
        ALOGE("The Windows Bluetooth bridge disconnected");
        std::lock_guard<std::mutex> guard(mStateMutex);
        mBridgeLost = true;
        PublishBridge(false);
      });
  mReaderStop = eventfd(0, EFD_CLOEXEC);
  mReader = std::thread(&BluetoothHci::readerLoop, this, fd);

  {
    std::lock_guard<std::mutex> guard(mStateMutex);
    mState = HalState::ONE_CLIENT;
  }
  mStateChanged.notify_all();
  wake();  // the connection is the reader's now
  ALOGI("initialization complete");
  auto status = mCb->initializationComplete(Status::SUCCESS);
  if (!status.isOk()) {
    if (!mDeathRecipient->getHasDied()) {
      ALOGE("Error sending init callback, but no death notification");
    }
    close();
    return ndk::ScopedAStatus::fromServiceSpecificError(STATUS_FAILED_TRANSACTION);
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::close() {
  ALOGI(__func__);
  {
    std::lock_guard<std::mutex> guard(mStateMutex);
    if (mState != HalState::ONE_CLIENT) {
      LOG_ALWAYS_FATAL_IF(mState == HalState::INITIALIZING, "mState is INITIALIZING");
      ALOGI("Already closed");
      return ndk::ScopedAStatus::ok();
    }
    mState = HalState::CLOSING;
  }

  stopReader();

  {
    std::lock_guard<std::mutex> guard(mStateMutex);
    if (mBridgeLost) {
      dropBridgeLocked();
      mBridgeLost = false;
    } else if (mFd >= 0) {
      // The stack is gone: a reset ends the bridge's scanning and connections.
      const uint8_t reset[] = {0x01, 0x03, 0x0c, 0x00};
      if (TEMP_FAILURE_RETRY(write(mFd, reset, sizeof reset)) != static_cast<ssize_t>(sizeof reset)) dropBridgeLocked();
    }
    if (mPendingFd >= 0) {
      // A bridge connected again while the stack used the old connection: take the new one.
      if (mFd >= 0) ::close(mFd);
      mFd = mPendingFd;
      mPendingFd = -1;
      PublishBridge(true);
    }
    mState = HalState::READY;
    mH4 = nullptr;
  }
  mStateChanged.notify_all();
  wake();  // idle again: serveBridge watches the connection
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::sendHciCommand(const std::vector<uint8_t>& packet) {
  return send(PacketType::COMMAND, packet);
}

ndk::ScopedAStatus BluetoothHci::sendAclData(const std::vector<uint8_t>& packet) {
  return send(PacketType::ACL_DATA, packet);
}

ndk::ScopedAStatus BluetoothHci::sendScoData(const std::vector<uint8_t>& packet) {
  return send(PacketType::SCO_DATA, packet);
}

ndk::ScopedAStatus BluetoothHci::sendIsoData(const std::vector<uint8_t>& packet) {
  return send(PacketType::ISO_DATA, packet);
}

ndk::ScopedAStatus BluetoothHci::send(PacketType type, const std::vector<uint8_t>& v) {
  if (v.empty()) {
    ALOGE("Packet is empty, no data was found to be sent");
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  }
  std::lock_guard<std::mutex> guard(mStateMutex);
  if (mH4 == nullptr || mFd < 0) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
  }
  mH4->Send(type, v);
  return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::bluetooth::impl

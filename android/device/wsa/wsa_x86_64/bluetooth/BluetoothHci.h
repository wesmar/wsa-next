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

// Bluetooth HCI HAL for WSA. Based on hardware/interfaces/bluetooth/aidl/default; the
// controller is not a UART but the Bluetooth bridge on the Windows host, which connects to this
// HAL over a Hyper-V socket (vsock port "WSBT") when it starts. HCI packets travel in H4 framing
// (one packet type byte, then the packet) in both directions.
//
// Everything is event driven: one thread waits (poll, no timeout) for the bridge to connect, for
// an idle connection to end and for state changes (eventfd); while the Bluetooth stack uses the
// HAL, a reader thread waits on the connection. The HAL publishes whether a bridge is connected
// in sys.wsa.bluetooth.bridge ("1" / "0"); the WSA services in system_server turn that into the
// no_bluetooth user restriction, so without the bridge Android offers no Bluetooth at all
// instead of a stack that cannot start.

#pragma once

#include <aidl/android/hardware/bluetooth/BnBluetoothHci.h>
#include <aidl/android/hardware/bluetooth/IBluetoothHciCallbacks.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "h4_protocol.h"

namespace aidl::android::hardware::bluetooth::impl {

class BluetoothDeathRecipient;

class BluetoothHci : public BnBluetoothHci {
 public:
  // vsock port of the HAL: "WSBT" (the Windows bridge connects to it).
  static constexpr unsigned int kBridgePort = 0x57534254;

  BluetoothHci();

  ndk::ScopedAStatus initialize(const std::shared_ptr<IBluetoothHciCallbacks>& cb) override;
  ndk::ScopedAStatus sendHciCommand(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendAclData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendScoData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendIsoData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus close() override;

 private:
  // Accepts bridge connections and watches an idle one; never returns.
  void serveBridge();
  // Makes serveBridge look at the state again (eventfd).
  void wake();
  // The connection is gone (call with mStateMutex held).
  void dropBridgeLocked();
  // Reads the bridge connection into mH4 while the stack uses it, until stopReader().
  void readerLoop(int fd);
  void stopReader();

  [[nodiscard]] ndk::ScopedAStatus send(::android::hardware::bluetooth::hci::PacketType type,
                                        const std::vector<uint8_t>& packet);

  int mFd{-1};         // the bridge connection, -1 while there is none
  int mPendingFd{-1};  // a new connection that arrived while the stack still used the old one
  int mWake{-1};       // eventfd for serveBridge
  std::shared_ptr<IBluetoothHciCallbacks> mCb;
  std::shared_ptr<::android::hardware::bluetooth::hci::H4Protocol> mH4;
  std::shared_ptr<BluetoothDeathRecipient> mDeathRecipient;
  std::thread mReader;
  int mReaderStop{-1};      // eventfd that ends readerLoop
  // The reader saw the bridge go; the fd is closed when the stack lets go. Atomic: the reader
  // checks it without mStateMutex, which a send() blocked in write() may hold - the reader must
  // keep draining the connection then, or both sides could wait for each other.
  std::atomic<bool> mBridgeLost{false};
  std::condition_variable mStateChanged;

  // Don't close twice or open before close is complete.
  std::mutex mStateMutex;
  enum class HalState {
    READY,
    INITIALIZING,
    ONE_CLIENT,
    CLOSING,
  } mState{HalState::READY};
};

}  // namespace aidl::android::hardware::bluetooth::impl

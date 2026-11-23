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

// Root for apps, confirmed on Windows with Windows Hello.
//
//   0 (in the app's process)  --unix socket /dev/socket/wsa_elevate-->  wsa-elevated (daemon)
//   wsa-elevated  --vsock "WSEV"-->  WSA-Next.exe host (Windows Hello with the app and the command)
//   granted: wsa-elevated starts wsa-elevate-exec, whose executable enters the elevated domain;
//   it takes the client's terminal or stdio from the client process itself (pidfd_getfd) and
//   runs the command. The client relays the terminal, the exit status and, without a terminal,
//   signals - no descriptor of the app ever passes through the daemon.
//
// The daemon learns who asks from the kernel (SO_PEERCRED), never from the request.
#pragma once

#include <stdint.h>

namespace wsa::elevate {

constexpr char kSocketName[] = "wsa_elevate";  // /dev/socket/wsa_elevate, created by init
constexpr char kSocketPath[] = "/dev/socket/wsa_elevate";
constexpr char kLauncher[] = "/system_ext/bin/wsa-elevate-exec";

constexpr uint32_t kRequestMagic = 0x4c455357;  // 'W','S','E','L'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxStrings = 256 * 1024;
constexpr uint32_t kMaxCount = 4096;

// client -> daemon, followed by `length` bytes: cwd\0, argc arguments\0, envc variables\0.
struct Request {
  uint32_t magic = kRequestMagic;
  uint32_t version = kVersion;
  int32_t fds[3] = {0, 1, 2};  // the client's descriptors that become 0, 1 and 2 of the command
  int32_t terminal = 0;        // 1: fds[0] is a terminal of its own (a pty) to make controlling
  uint32_t argc = 0;           // 0: an interactive shell
  uint32_t envc = 0;
  uint32_t length = 0;
};

// daemon -> client: one byte; kExited is followed by the int32 wait status.
enum Reply : uint8_t {
  kGranted = 'G',
  kDenied = 'D',
  kUnavailable = 'U',  // root for Android apps is off in WSA-Next, or its host is not running
  kBusy = 'B',
  kNoHello = 'H',      // Windows Hello is not set up on Windows
  kInvalid = 'I',
  kFailed = 'F',
  kExited = 'X',
};

// client -> daemon after kGranted (no terminal of its own): kSignal, then the int32 signal.
constexpr uint8_t kSignal = 'S';

// The Google Services Framework ID for WSA-Next's "Register this PC with Google Play"
// notification (device/wsa/wsa_x86_64/registration). Current Play services give it only to
// Google-signed apps, so that app asks here: client -> daemon the four bytes "GFID"; daemon ->
// client kGranted, one length byte and the decimal ID, or kDenied / kFailed. No Windows Hello:
// it is the ID the user copies to Google's registration page, given to that one app only.
constexpr uint32_t kGsfIdMagic = 0x44494647;  // 'G','F','I','D'
constexpr char kRegistrationPackage[] = "org.wsanext.registration";

// The same app asks for the notice on Windows: a click on Android's own notification does not
// come back to Android there. Client -> daemon "RGWN"; the daemon reads the ID itself and hands it
// to WSA-Next on Windows over vsock port "WSRG" (daemon -> host "RGID", one length byte, the
// decimal ID; host -> daemon one byte), whose notification copies the ID and opens Google's page
// in the Windows browser. Daemon -> client: that byte - kRegistrationShown, kRegistrationDone
// (registered from Windows before), kRegistrationNotShown (no icon to show it) - or kUnavailable
// (no WSA-Next host), kDenied, kFailed (no ID yet).
constexpr uint32_t kRegisterOnWindowsMagic = 0x4E574752;  // 'R','G','W','N'
constexpr uint32_t kRegistrationPort = 0x57535247;        // "WSRG"
constexpr uint8_t kRegistrationShown = 'S';
constexpr uint8_t kRegistrationDone = 'O';  // not 'D': kDenied
constexpr uint8_t kRegistrationNotShown = 'N';
constexpr char kGsfIdOption[] = "--gsf-id";  // wsa-elevate-exec --gsf-id prints it

}  // namespace wsa::elevate

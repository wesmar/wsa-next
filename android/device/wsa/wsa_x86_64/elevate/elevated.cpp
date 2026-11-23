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

// wsa-elevated: decides nothing itself. It asks the elevation bridge of WSA-Next.exe on Windows
// (vsock port "WSEV"), which shows Windows Hello with the app and the command, and starts the
// command as root only on a verified answer. Without the bridge (root for Android apps turned
// off on Windows) every request is refused. All waits are on file descriptors, without timeouts.
#define LOG_TAG "wsa-elevated"

#include <cutils/sockets.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>  // before linux/vm_sockets.h, which needs struct sockaddr
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "log/log.h"
#include "protocol.h"

namespace wsa::elevate {
namespace {

// Host protocol (src/biometrics/ElevationProtocol.h on Windows).
constexpr uint32_t kBridgePort = 0x57534556;        // "WSEV"
constexpr uint32_t kHostMagic = 0x56455357;         // 'W','S','E','V'
constexpr uint8_t kOpRequest = 1, kOpAnswer = 2, kOpCancel = 3, kOpConfig = 4;
constexpr uint8_t kGrantedStatus = 1, kDeniedStatus = 2, kNoHelloStatus = 3, kBusyStatus = 4;
constexpr uint8_t kNoBridge = 0xFF;  // local: no bridge connected, or it went away
constexpr size_t kMaxShown = 1024;   // of the command sent to Windows
constexpr char kSuLink[] = "/dev/wsa/bin/su";  // on everyone's PATH (wsa-elevated.rc)
constexpr char kZero[] = "/system_ext/bin/0";

// "su for root apps": the link exists exactly while a bridge that wants it is connected.
void SetSuLink(bool present) {
  if (present) {
    if (symlink(kZero, kSuLink) == 0) ALOGI("su for root apps: on");
    else if (errno != EEXIST) ALOGE("Cannot create %s: %s", kSuLink, strerror(errno));
  } else if (unlink(kSuLink) == 0) {
    ALOGI("su for root apps: off");
  }
}

struct HostFrame {
  uint32_t magic = kHostMagic;
  uint8_t op = 0;
  uint8_t status = 0;
  uint16_t payloadLen = 0;
};
static_assert(sizeof(HostFrame) == 8);

bool SendAll(int fd, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size) {
    const ssize_t n = TEMP_FAILURE_RETRY(send(fd, p, size, MSG_NOSIGNAL));
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool RecvAll(int fd, void* data, size_t size) {
  auto* p = static_cast<uint8_t*>(data);
  while (size) {
    const ssize_t n = TEMP_FAILURE_RETRY(recv(fd, p, size, 0));
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

void PutU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void PutText(std::string& out, const std::string& text) {
  const size_t n = std::min<size_t>(text.size(), kMaxShown);
  out += static_cast<char>(n & 0xFF);
  out += static_cast<char>(n >> 8);
  out.append(text, 0, n);
}

// The connection from the elevation bridge, and the one request it is deciding.
class HostLink {
 public:
  HostLink() : mAnswerEvent(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

  void Start() { std::thread(&HostLink::Serve, this).detach(); }

  // Asks Windows. While waiting, a client that goes away cancels the dialog.
  uint8_t Ask(uint32_t uid, const std::string& packages, const std::string& command, int client) {
    std::lock_guard<std::mutex> one(mAskLock);  // one dialog at a time
    uint32_t id = 0;
    {
      std::lock_guard<std::mutex> guard(mLock);
      if (mConnection < 0) return kNoBridge;
      id = ++mNextId;
      mPending = id;
      mAnswer.reset();
      uint64_t drain = 0;
      (void)TEMP_FAILURE_RETRY(read(mAnswerEvent, &drain, sizeof drain));  // a late answer of an earlier request
      std::string payload;
      PutU32(payload, id);
      PutU32(payload, uid);
      PutText(payload, packages);
      PutText(payload, command);
      const HostFrame frame{kHostMagic, kOpRequest, 0, static_cast<uint16_t>(payload.size())};
      if (!SendAll(mConnection, &frame, sizeof frame) || !SendAll(mConnection, payload.data(), payload.size())) {
        mPending = 0;
        return kNoBridge;
      }
    }
    bool canceled = false;
    for (;;) {
      pollfd fds[2] = {{mAnswerEvent, POLLIN, 0}, {canceled ? -1 : client, POLLRDHUP, 0}};
      if (TEMP_FAILURE_RETRY(poll(fds, 2, -1)) < 0) return kNoBridge;
      if (fds[0].revents & POLLIN) break;
      if (fds[1].revents) {
        canceled = true;  // the client gave up: close the dialog, its answer still ends the wait
        std::lock_guard<std::mutex> guard(mLock);
        std::string payload;
        PutU32(payload, id);
        const HostFrame frame{kHostMagic, kOpCancel, 0, static_cast<uint16_t>(payload.size())};
        if (mConnection >= 0) (void)(SendAll(mConnection, &frame, sizeof frame) && SendAll(mConnection, payload.data(), payload.size()));
      }
    }
    std::lock_guard<std::mutex> guard(mLock);
    mPending = 0;
    return canceled ? kDeniedStatus : mAnswer.value_or(kNoBridge);
  }

 private:
  void AnswerLocked(uint8_t status) {
    mAnswer = status;
    const uint64_t one = 1;
    (void)TEMP_FAILURE_RETRY(write(mAnswerEvent, &one, sizeof one));
  }

  void DropLocked() {
    if (mConnection >= 0) ::close(mConnection);
    mConnection = -1;
    SetSuLink(false);
    if (mPending) AnswerLocked(kNoBridge);
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
      pollfd fds[2] = {{listener, POLLIN, 0}, {connection, POLLIN | POLLRDHUP, 0}};
      if (TEMP_FAILURE_RETRY(poll(fds, 2, -1)) < 0) continue;
      if (fds[0].revents & POLLIN) {
        const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd >= 0) {
          std::lock_guard<std::mutex> guard(mLock);
          DropLocked();  // a new bridge replaces the previous connection
          mConnection = fd;
          ALOGI("The elevation bridge connected");
        }
        continue;
      }
      if (connection < 0 || fds[1].revents == 0) continue;
      HostFrame frame{};
      uint8_t body[4] = {};
      // Data first: a last frame may come with the hangup (read until recv says the end).
      bool ok = (fds[1].revents & POLLIN) && RecvAll(connection, &frame, sizeof frame) && frame.magic == kHostMagic &&
                frame.payloadLen <= 64;
      for (uint16_t i = 0; ok && i < frame.payloadLen; ++i) {
        uint8_t byte = 0;
        ok = RecvAll(connection, &byte, 1);
        if (i < 4) body[i] = byte;
      }
      std::lock_guard<std::mutex> guard(mLock);
      if (connection != mConnection) continue;
      if (!ok) {
        ALOGI("The elevation bridge disconnected");
        DropLocked();
        continue;
      }
      const uint32_t id = body[0] | (body[1] << 8) | (body[2] << 16) | (static_cast<uint32_t>(body[3]) << 24);
      if (frame.op == kOpAnswer && frame.payloadLen >= 4 && mPending && id == mPending) AnswerLocked(frame.status);
      else if (frame.op == kOpConfig) SetSuLink(frame.status == 1);
    }
  }

  std::mutex mAskLock, mLock;
  int mConnection = -1;
  const int mAnswerEvent;
  uint32_t mNextId = 0, mPending = 0;
  std::optional<uint8_t> mAnswer;
};

HostLink& Host() {
  static HostLink* link = new HostLink();  // lives as long as the daemon
  return *link;
}

// The always-on link to WSA-Next on Windows for Google Play registration (vsock "WSRG",
// protocol.h): the host connects whenever it runs, whether root for apps is on or not.
class RegistrationLink {
 public:
  void Start() { std::thread(&RegistrationLink::Serve, this).detach(); }

  // Hands the device ID to the host; its one-byte answer, or kUnavailable without a host.
  uint8_t Offer(const std::string& id) {
    std::lock_guard<std::mutex> guard(mLock);
    if (mConnection < 0) return kUnavailable;
    std::string message = "RGID";
    message += static_cast<char>(id.size());
    message += id;
    uint8_t reply = 0;
    if (!SendAll(mConnection, message.data(), message.size()) || !RecvAll(mConnection, &reply, 1)) {
      ::close(mConnection);
      mConnection = -1;
      return kUnavailable;
    }
    return reply;
  }

 private:
  void Serve() {
    const int listener = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_vm addr{};
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = VMADDR_CID_ANY;
    addr.svm_port = kRegistrationPort;
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listener, 1) != 0) {
      ALOGE("Cannot listen on registration vsock port 0x%08x: %s", kRegistrationPort, strerror(errno));
      return;
    }
    for (;;) {
      const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) continue;
      std::lock_guard<std::mutex> guard(mLock);
      if (mConnection >= 0) ::close(mConnection);
      mConnection = fd;
      ALOGI("The registration helper connected");
    }
  }

  std::mutex mLock;
  int mConnection = -1;
};

RegistrationLink& RegistrationHost() {
  static RegistrationLink* link = new RegistrationLink();
  return *link;
}

// The packages of an app uid ("com.termux"; several for a shared uid), from packages.list.
std::string Packages(uid_t uid) {
  if (uid == 0) return "root";
  if (uid == 2000) return "shell";
  const uid_t appId = uid % 100000;
  std::ifstream list("/data/system/packages.list");
  std::string line, found;
  while (std::getline(list, line)) {
    std::istringstream fields(line);
    std::string name;
    uid_t id = 0;
    if (!(fields >> name >> id) || id != appId) continue;
    found += (found.empty() ? "" : ",") + name;
  }
  return found;
}

// The command as the user will read it on Windows.
std::string Shown(const std::vector<std::string>& argv) {
  std::string out;
  for (const std::string& arg : argv) {
    if (!out.empty()) out += ' ';
    const bool quote = arg.empty() || arg.find_first_of(" \t\"'\\$`") != std::string::npos;
    if (!quote) {
      out += arg;
      continue;
    }
    out += '\'';
    for (const char c : arg) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    out += '\'';
  }
  return out;
}

std::string EnvValue(const std::vector<std::string>& env, const std::string& name) {
  for (const std::string& entry : env)
    if (entry.size() > name.size() && entry.compare(0, name.size(), name) == 0 && entry[name.size()] == '=') return entry.substr(name.size() + 1);
  return {};
}

// A program name as the client's shell would find it: absolute, relative to cwd, or on its PATH.
std::string Resolve(const std::string& program, const std::vector<std::string>& env, const std::string& cwd) {
  if (program.find('/') != std::string::npos) return program[0] == '/' ? program : cwd + "/" + program;
  std::string path = EnvValue(env, "PATH");
  if (path.empty()) path = "/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin";
  std::istringstream dirs(path);
  std::string dir;
  while (std::getline(dirs, dir, ':')) {
    if (dir.empty()) continue;
    const std::string candidate = dir + "/" + program;
    if (access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return "/system/bin/" + program;
}

struct ClientRequest {
  Request header;
  std::string cwd;
  std::vector<std::string> argv, env;
};

// The rest of a request whose first field (the magic) the caller has read.
std::optional<ClientRequest> ReadRequest(int fd, uint32_t magic) {
  ClientRequest r;
  r.header.magic = magic;
  static_assert(offsetof(Request, magic) == 0);
  if (!RecvAll(fd, reinterpret_cast<uint8_t*>(&r.header) + sizeof magic, sizeof r.header - sizeof magic)) return std::nullopt;
  const Request& h = r.header;
  if (h.magic != kRequestMagic || h.version != kVersion || h.length > kMaxStrings || h.argc > kMaxCount || h.envc > kMaxCount)
    return std::nullopt;
  for (const int32_t f : h.fds)
    if (f < 0) return std::nullopt;
  std::string strings(h.length, '\0');
  if (h.length && !RecvAll(fd, strings.data(), strings.size())) return std::nullopt;
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < strings.size()) {
    const size_t end = strings.find('\0', start);
    if (end == std::string::npos) return std::nullopt;
    parts.emplace_back(strings, start, end - start);
    start = end + 1;
  }
  if (parts.size() != 1 + h.argc + h.envc) return std::nullopt;
  r.cwd = parts[0].empty() ? "/" : parts[0];
  r.argv.assign(parts.begin() + 1, parts.begin() + 1 + h.argc);
  r.env.assign(parts.begin() + 1 + h.argc, parts.end());
  return r;
}

void SendReply(int fd, uint8_t code) { (void)SendAll(fd, &code, 1); }

// The Google Services Framework ID (decimal), or empty before Play services have checked in.
// It lives in Play services' own data (shared_prefs/Checkin.xml), which this narrow domain
// cannot read: the launcher reads it in the elevated domain and prints nothing but the number.
std::string ReadGsfId() {
  int out[2];
  if (pipe2(out, O_CLOEXEC) != 0) return {};
  const pid_t child = fork();
  if (child == 0) {
    if (dup2(out[1], 1) < 0) _exit(126);  // dup2 clears close-on-exec
    syscall(__NR_close_range, 3, ~0U, 0);
    char launcher[] = "wsa-elevate-exec", option[sizeof kGsfIdOption];
    memcpy(option, kGsfIdOption, sizeof option);
    char* argv[] = {launcher, option, nullptr};
    char* envp[] = {nullptr};
    execve(kLauncher, argv, envp);
    _exit(127);
  }
  close(out[1]);
  std::string id;
  char buffer[32];
  ssize_t n = 0;
  while (id.size() <= 20 && (n = TEMP_FAILURE_RETRY(read(out[0], buffer, sizeof buffer))) > 0) id.append(buffer, static_cast<size_t>(n));
  close(out[0]);
  int status = 0;
  const bool exited = child > 0 && TEMP_FAILURE_RETRY(waitpid(child, &status, 0)) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!exited || id.empty() || id.size() > 20 || id.find_first_not_of("0123456789") != std::string::npos) return {};
  return id;  // empty above: Play services have not checked in yet, or there are none
}

// Only the registration app may have the ID or offer it on Windows (protocol.h).
bool FromRegistrationApp(int client, uid_t uid, const char* what) {
  if (Packages(uid) == kRegistrationPackage) return true;
  ALOGW("uid %u asked to %s: refused (only %s may)", uid, what, kRegistrationPackage);
  SendReply(client, kDenied);
  return false;
}

void ServeGsfId(int client, uid_t uid) {
  if (!FromRegistrationApp(client, uid, "read the device ID")) return;
  const std::string id = ReadGsfId();
  if (id.empty()) {
    SendReply(client, kFailed);
    return;
  }
  std::string reply(1, static_cast<char>(kGranted));
  reply += static_cast<char>(id.size());
  reply += id;
  (void)SendAll(client, reply.data(), reply.size());
}

void ServeRegisterOnWindows(int client, uid_t uid) {
  if (!FromRegistrationApp(client, uid, "offer the registration on Windows")) return;
  const std::string id = ReadGsfId();
  SendReply(client, id.empty() ? kFailed : RegistrationHost().Offer(id));
}

void Serve(int client) {
  ucred peer{};
  socklen_t size = sizeof peer;
  if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0) return;
  uint32_t magic = 0;
  if (!RecvAll(client, &magic, sizeof magic)) return;
  if (magic == kGsfIdMagic) {
    ServeGsfId(client, peer.uid);
    return;
  }
  if (magic == kRegisterOnWindowsMagic) {
    ServeRegisterOnWindows(client, peer.uid);
    return;
  }
  // A reference to exactly this client process (its pid cannot be reused while held).
  const int pidfd = static_cast<int>(syscall(__NR_pidfd_open, peer.pid, 0));
  if (pidfd < 0) return;
  const auto request = ReadRequest(client, magic);
  if (!request) {
    SendReply(client, kInvalid);
    close(pidfd);
    return;
  }
  // The pid was the client's when it connected; its socket still open means it still is.
  pollfd alive{client, POLLRDHUP, 0};
  if (TEMP_FAILURE_RETRY(poll(&alive, 1, 0)) != 0) {
    close(pidfd);
    return;
  }

  const std::string packages = Packages(peer.uid);
  const std::string shown = request->argv.empty() ? std::string() : Shown(request->argv);
  ALOGI("uid %u (%s) asks for root: %s", peer.uid, packages.empty() ? "?" : packages.c_str(), shown.empty() ? "<interactive shell>" : shown.c_str());
  // A process that is root in the elevated domain already gains nothing: no second question.
  // Its context comes from the kernel (SO_PEERSEC), like its uid.
  char context[256] = {};
  socklen_t contextSize = sizeof context - 1;
  const bool elevated = peer.uid == 0 && getsockopt(client, SOL_SOCKET, SO_PEERSEC, context, &contextSize) == 0 &&
                        strncmp(context, "u:r:su:", 7) == 0;
  const uint8_t answer = elevated ? kGrantedStatus : Host().Ask(peer.uid, packages, shown, client);
  ALOGI("uid %u: %s", peer.uid,
        answer == kGrantedStatus ? "granted" : answer == kNoBridge ? "refused (no elevation bridge)" : answer == kNoHelloStatus ? "refused (no Windows Hello)" : "denied");
  if (answer != kGrantedStatus) {
    SendReply(client, answer == kNoBridge ? kUnavailable : answer == kNoHelloStatus ? kNoHello : answer == kBusyStatus ? kBusy : kDenied);
    close(pidfd);
    return;
  }

  // The command line of the launcher: <pidfd> <fd0> <fd1> <fd2> <terminal> <cwd> <program> args...
  std::vector<std::string> program = request->argv;
  if (program.empty()) {
    const std::string shell = EnvValue(request->env, "SHELL");
    program.push_back(!shell.empty() && shell[0] == '/' && access(shell.c_str(), X_OK) == 0 ? shell : "/system/bin/sh");
  }
  program[0] = Resolve(program[0], request->env, request->cwd);
  std::vector<std::string> args = {"wsa-elevate-exec", "3", std::to_string(request->header.fds[0]), std::to_string(request->header.fds[1]),
                                   std::to_string(request->header.fds[2]), request->header.terminal ? "1" : "0", request->cwd};
  args.insert(args.end(), program.begin(), program.end());
  std::vector<char*> argv, envp;
  for (std::string& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  std::vector<std::string> env = request->env;
  for (std::string& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);

  const pid_t child = fork();
  if (child < 0) {
    SendReply(client, kFailed);
    close(pidfd);
    return;
  }
  if (child == 0) {
    // The launcher keeps the client's pidfd as descriptor 3 and nothing else of the daemon.
    if (dup2(pidfd, 3) < 0) _exit(126);
    syscall(__NR_close_range, 4, ~0U, 0);
    execve(kLauncher, argv.data(), envp.data());
    _exit(127);
  }
  close(pidfd);
  SendReply(client, kGranted);

  // Until the command ends: the client's signals go to its process group; a client that goes
  // away hangs it up, as a closed terminal would.
  const int childfd = static_cast<int>(syscall(__NR_pidfd_open, child, 0));
  bool clientGone = false;
  for (;;) {
    pollfd fds[2] = {{childfd, POLLIN, 0}, {clientGone ? -1 : client, POLLIN | POLLRDHUP, 0}};
    if (childfd < 0) break;  // cannot watch: wait for it below
    if (TEMP_FAILURE_RETRY(poll(fds, 2, -1)) < 0) break;
    if (fds[0].revents) break;
    if (!fds[1].revents) continue;
    uint8_t type = 0;
    int32_t number = 0;
    if ((fds[1].revents & POLLIN) && RecvAll(client, &type, 1) && type == kSignal && RecvAll(client, &number, sizeof number)) {
      static constexpr int kAllowed[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGKILL, SIGCONT, SIGTSTP, SIGWINCH, SIGUSR1, SIGUSR2};
      if (std::find(std::begin(kAllowed), std::end(kAllowed), number) != std::end(kAllowed) && kill(-child, number) != 0) kill(child, number);
      continue;
    }
    clientGone = true;
    if (kill(-child, SIGHUP) != 0) kill(child, SIGHUP);
  }
  int status = 0;
  (void)TEMP_FAILURE_RETRY(waitpid(child, &status, 0));
  if (childfd >= 0) close(childfd);
  if (!clientGone) {
    const uint8_t exited = kExited;
    const int32_t code = status;
    (void)(SendAll(client, &exited, 1) && SendAll(client, &code, sizeof code));
  }
  ALOGI("uid %u: the elevated command ended (status 0x%x)", peer.uid, status);
}

}  // namespace
}  // namespace wsa::elevate

int main() {
  using namespace wsa::elevate;
  signal(SIGPIPE, SIG_IGN);
  const int listener = android_get_control_socket(kSocketName);
  if (listener < 0 || listen(listener, 8) != 0) {
    ALOGE("No control socket %s: %s", kSocketName, strerror(errno));
    return 1;
  }
  SetSuLink(false);  // until a bridge says otherwise
  Host().Start();
  RegistrationHost().Start();
  ALOGI("ready");
  for (;;) {
    const int client = TEMP_FAILURE_RETRY(accept4(listener, nullptr, nullptr, SOCK_CLOEXEC));
    if (client < 0) continue;
    std::thread([client] {
      Serve(client);
      close(client);
    }).detach();
  }
}

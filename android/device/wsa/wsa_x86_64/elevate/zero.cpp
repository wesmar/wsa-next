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

// 0: runs a command (or a shell) as root - uid 0 - once Windows Hello on the PC says yes.
//
//   0                  an interactive root shell
//   0 -c "command"     the command through sh
//   0 program [args]   the program
//
// Called as "su" (/dev/wsa/bin/su, present only while "su for root apps" is on in WSA-Next) it
// speaks su's syntax, so root apps and libsu work unchanged.
//
// On a terminal the root command gets a terminal of its own (a pty this process relays), so
// line editing, job control and Ctrl+C behave as usual. Exits with the command's status.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "protocol.h"

extern char** environ;

namespace {

using namespace wsa::elevate;

termios gSaved;
bool gRaw = false;

void Restore() {
  if (gRaw) tcsetattr(STDIN_FILENO, TCSAFLUSH, &gSaved);
  gRaw = false;
}

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* p = static_cast<const char*>(data);
  while (size) {
    const ssize_t n = TEMP_FAILURE_RETRY(write(fd, p, size));
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t size) {
  auto* p = static_cast<char*>(data);
  while (size) {
    const ssize_t n = TEMP_FAILURE_RETRY(read(fd, p, size));
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

int Usage() {
  fprintf(stderr,
          "usage: 0                  interactive root shell\n"
          "       0 -c \"command\"     run the command through sh\n"
          "       0 program [args]   run the program\n"
          "Each request is confirmed on Windows with Windows Hello (WSA-Next: Android > Root for apps).\n");
  return 2;
}

int Refused(uint8_t reply) {
  const char* why = reply == kDenied        ? "denied on Windows"
                    : reply == kUnavailable ? "root for Android apps is off (WSA-Next on Windows: Android > Root for apps)"
                    : reply == kBusy        ? "another request is waiting for Windows Hello"
                    : reply == kNoHello     ? "Windows Hello is not set up on Windows"
                    : reply == kInvalid     ? "invalid request"
                                            : "failed";
  fprintf(stderr, "0: %s\n", why);
  return 1;
}

int ExitCode(int32_t status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

// Called as "su" (the link WSA-Next makes for root apps while "su for root apps" is on): the
// syntax root apps and libraries such as libsu use. Returns an exit code, or -1 to go on.
//   su [options] [-] [0|root] [command [args...]]
//   -c|--command cmd, -s|--shell sh, -v|--version, -V; accepted and needless here: -, -l,
//   --login, -p, -m, --preserve-environment, --mount-master, -mm, -M, -Z|--context ctx
int ParseSu(int argc, char** argv, std::vector<std::string>& command) {
  std::string shell, run;
  bool hasRun = false;
  int i = 1;
  for (; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-c" || a == "--command" || a == "-s" || a == "--shell" || a == "-Z" || a == "--context") {
      if (++i >= argc) return Usage();
      if (a == "-c" || a == "--command") run = argv[i], hasRun = true;
      else if (a == "-s" || a == "--shell") shell = argv[i];
    } else if (a.rfind("--command=", 0) == 0) {
      run = a.substr(10), hasRun = true;
    } else if (a == "-" || a == "-l" || a == "--login" || a == "-p" || a == "-m" || a == "--preserve-environment" ||
               a == "--mount-master" || a == "-mm" || a == "-M") {
      // The environment is passed anyway; there is one mount namespace view to give.
    } else if (a == "-v" || a == "--version") {
      printf("0:WSA-Next\n");
      return 0;
    } else if (a == "-V") {
      printf("1\n");
      return 0;
    } else if (a == "-h" || a == "--help") {
      return Usage();
    } else if (a == "--") {
      ++i;
      break;
    } else if (!a.empty() && a[0] == '-') {
      fprintf(stderr, "su: unknown option %s\n", a.c_str());
      return 2;
    } else {
      break;
    }
  }
  if (i < argc) {  // the user: there is one, root
    if (strcmp(argv[i], "0") != 0 && strcmp(argv[i], "root") != 0) {
      fprintf(stderr, "su: only root (0)\n");
      return 1;
    }
    ++i;
  }
  if (hasRun) command = {shell.empty() ? "sh" : shell, "-c", run};
  else if (i < argc) command.assign(argv + i, argv + argc);  // "su 0 command args", as AOSP's su
  else if (!shell.empty()) command = {shell};
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> command;
  const char* name = strrchr(argv[0], '/');
  name = name ? name + 1 : argv[0];
  if (!strcmp(name, "su")) {
    if (const int code = ParseSu(argc, argv, command); code >= 0) return code;
  } else if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
    return Usage();
  } else if (argc >= 2 && !strcmp(argv[1], "-c")) {
    if (argc != 3) return Usage();
    command = {"sh", "-c", argv[2]};
  } else {
    for (int i = (argc >= 2 && !strcmp(argv[1], "--")) ? 2 : 1; i < argc; ++i) command.emplace_back(argv[i]);
  }

  const int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, kSocketPath, sizeof addr.sun_path - 1);
  if (sock < 0 || connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    fprintf(stderr, "0: the elevation service is not available (%s)\n", strerror(errno));
    return 1;
  }

  // A terminal of its own for the root command when this one runs on a terminal.
  const bool onTerminal = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
  int master = -1, slave = -1;
  if (onTerminal) {
    char name[64] = {};
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || ptsname_r(master, name, sizeof name) != 0 ||
        (slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) {
      fprintf(stderr, "0: no pseudo-terminal: %s\n", strerror(errno));
      return 1;
    }
    termios mode;
    if (tcgetattr(STDIN_FILENO, &mode) == 0) tcsetattr(slave, TCSANOW, &mode);
    winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) ioctl(master, TIOCSWINSZ, &size);
  }

  Request request;
  if (onTerminal) {
    request.fds[0] = request.fds[1] = slave;
    request.fds[2] = isatty(STDERR_FILENO) ? slave : STDERR_FILENO;
    request.terminal = 1;
  }
  char cwd[4096] = "/";
  if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
  std::string strings(cwd);
  strings += '\0';
  for (const std::string& arg : command) strings += arg + '\0';
  uint32_t envc = 0;
  for (char** e = environ; e && *e; ++e, ++envc) strings += std::string(*e) + '\0';
  request.argc = static_cast<uint32_t>(command.size());
  request.envc = envc;
  request.length = static_cast<uint32_t>(strings.size());
  if (strings.size() > kMaxStrings || !WriteAll(sock, &request, sizeof request) || !WriteAll(sock, strings.data(), strings.size())) {
    fprintf(stderr, "0: the request is too large or the service went away\n");
    return 1;
  }

  // Waits for Windows Hello. Signals stay deliverable meanwhile: Ctrl+C here cancels the request.
  uint8_t reply = 0;
  if (!ReadAll(sock, &reply, 1)) return Refused(kFailed);
  if (reply != kGranted) return Refused(reply);

  sigset_t watched;
  sigemptyset(&watched);
  for (const int s : {SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGCONT, SIGUSR1, SIGUSR2}) sigaddset(&watched, s);
  sigprocmask(SIG_BLOCK, &watched, nullptr);
  const int signals = signalfd(-1, &watched, SFD_CLOEXEC);

  if (onTerminal) {
    // The root command has taken the pty's other end: this one only relays, in raw mode.
    if (tcgetattr(STDIN_FILENO, &gSaved) == 0) {
      termios raw = gSaved;
      cfmakeraw(&raw);
      gRaw = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
      atexit(Restore);
    }
  }

  bool inputOpen = onTerminal, outputOpen = onTerminal;
  int32_t status = 0;
  bool exited = false;
  char buffer[16384];
  while (!exited) {
    pollfd fds[4] = {{sock, POLLIN, 0}, {signals, POLLIN, 0}, {inputOpen ? STDIN_FILENO : -1, POLLIN, 0}, {outputOpen ? master : -1, POLLIN, 0}};
    if (TEMP_FAILURE_RETRY(poll(fds, 4, -1)) < 0) break;
    if (fds[3].revents) {
      const ssize_t n = TEMP_FAILURE_RETRY(read(master, buffer, sizeof buffer));
      if (n > 0) WriteAll(STDOUT_FILENO, buffer, static_cast<size_t>(n));
      else outputOpen = false;  // EIO: nothing holds the terminal any more
    }
    if (fds[2].revents) {
      const ssize_t n = TEMP_FAILURE_RETRY(read(STDIN_FILENO, buffer, sizeof buffer));
      if (n > 0) WriteAll(master, buffer, static_cast<size_t>(n));
      else inputOpen = false;
    }
    if (fds[1].revents) {
      signalfd_siginfo info;
      if (ReadAll(signals, &info, sizeof info)) {
        const int number = static_cast<int>(info.ssi_signo);
        if (number == SIGWINCH && onTerminal) {
          winsize size;
          if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) ioctl(master, TIOCSWINSZ, &size);
        } else if (!onTerminal || number == SIGTERM || number == SIGHUP) {
          // On a terminal, Ctrl+C and friends are bytes for the pty; these come from outside.
          const uint8_t type = kSignal;
          const int32_t value = number;
          WriteAll(sock, &type, 1);
          WriteAll(sock, &value, sizeof value);
        }
      }
    }
    if (fds[0].revents) {
      uint8_t type = 0;
      if (ReadAll(sock, &type, 1) && type == kExited && ReadAll(sock, &status, sizeof status)) exited = true;
      else break;  // the service went away
    }
  }
  if (onTerminal && outputOpen) {
    // What the command wrote last is still in the pty.
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    for (ssize_t n; (n = read(master, buffer, sizeof buffer)) > 0;) WriteAll(STDOUT_FILENO, buffer, static_cast<size_t>(n));
  }
  Restore();
  return exited ? ExitCode(status) : 1;
}

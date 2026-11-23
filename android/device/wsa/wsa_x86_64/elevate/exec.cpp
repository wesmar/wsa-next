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

// wsa-elevate-exec: started by wsa-elevated only, after Windows Hello said yes. Its executable's
// label makes the exec enter the elevated domain (sepolicy wsa_elevate.te).
//
//   wsa-elevate-exec <pidfd> <fd0> <fd1> <fd2> <terminal 0|1> <cwd> <program> [arguments...]
//
// <pidfd> (inherited) refers to the client process; its descriptors <fd0..2> become 0, 1 and 2.
//
//   wsa-elevate-exec --gsf-id
//
// prints the Google Services Framework ID from Play services' Checkin.xml and nothing else
// (for WSA-Next's registration app, protocol.h); exits 1 while there is none.

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <string_view>

namespace {

int ParseInt(const char* text) {
  char* end = nullptr;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < -1 || value > 1 << 20) return -2;
  return static_cast<int>(value);
}

constexpr char kGmsPrefs[] = "/data/data/com.google.android.gms/shared_prefs";
constexpr char kCheckin[] = "/data/data/com.google.android.gms/shared_prefs/Checkin.xml";

// <string name="android_id">1234567890123456789</string> in Play services' check-in preferences;
// empty while they have not checked in (no file, no entry, or 0).
std::string ReadGsfId() {
  const int fd = open(kCheckin, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  std::string text;
  char buffer[4096];
  ssize_t n = 0;
  while (text.size() < 256 * 1024 && (n = TEMP_FAILURE_RETRY(read(fd, buffer, sizeof buffer))) > 0) text.append(buffer, static_cast<size_t>(n));
  close(fd);
  constexpr std::string_view kKey = "name=\"android_id\"";
  size_t p = text.find(kKey);
  if (p == std::string::npos) return {};
  p = text.find_first_of("0123456789<", p + kKey.size());  // past '>' or value="
  if (p == std::string::npos || text[p] == '<') return {};
  const size_t end = text.find_first_not_of("0123456789", p);
  std::string id = text.substr(p, end == std::string::npos ? std::string::npos : end - p);
  if (id.empty() || id.size() > 20 || id == "0") return {};
  return id;
}

// Play services announce CHECKIN_COMPLETE before their preferences reach the disk
// (SharedPreferences.apply), so the registration app may ask first: then this waits for
// Checkin.xml to be written (inotify), at most kWaitMs - the time a broadcast receiver has.
int PrintGsfId() {
  constexpr int kWaitMs = 20000;
  const int watch = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (watch >= 0) inotify_add_watch(watch, kGmsPrefs, IN_CLOSE_WRITE | IN_MOVED_TO);  // before the first read
  timespec start{};
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    const std::string id = ReadGsfId();
    if (!id.empty()) {
      if (watch >= 0) close(watch);
      return fwrite(id.data(), 1, id.size(), stdout) == id.size() && fflush(stdout) == 0 ? 0 : 1;
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    pollfd changed{watch, POLLIN, 0};
    if (watch < 0 || elapsed >= kWaitMs || TEMP_FAILURE_RETRY(poll(&changed, 1, static_cast<int>(kWaitMs - elapsed))) <= 0) {
      if (watch >= 0) close(watch);
      return 1;
    }
    char events[4096];
    while (read(watch, events, sizeof events) > 0) {
    }  // any change in the folder: read the file again
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (getuid() != 0) return 126;
  if (argc == 2 && strcmp(argv[1], "--gsf-id") == 0) return PrintGsfId();
  if (argc < 8) return 126;
  const int pidfd = ParseInt(argv[1]);
  const int fds[3] = {ParseInt(argv[2]), ParseInt(argv[3]), ParseInt(argv[4])};
  const bool terminal = strcmp(argv[5], "1") == 0;
  if (pidfd < 0 || fds[0] < 0 || fds[1] < 0 || fds[2] < 0) return 126;

  int taken[3];
  for (int i = 0; i < 3; ++i) {
    taken[i] = static_cast<int>(syscall(__NR_pidfd_getfd, pidfd, fds[i], 0));
    if (taken[i] < 0) return 125;  // the client went away
  }
  for (int i = 0; i < 3; ++i) {
    if (dup2(taken[i], i) < 0) return 125;
  }
  for (int i = 0; i < 3; ++i) {
    if (taken[i] > 2) close(taken[i]);
  }
  close(pidfd);

  // Root as root logs in: none of the daemon's supplementary groups.
  const gid_t rootGroup = 0;
  setgroups(1, &rootGroup);
  setsid();
  if (terminal) ioctl(0, TIOCSCTTY, 0);
  if (chdir(argv[6]) != 0 && chdir("/") != 0) return 126;
  execv(argv[7], argv + 7);
  fprintf(stderr, "0: %s: %s\n", argv[7], strerror(errno));
  return 127;
}

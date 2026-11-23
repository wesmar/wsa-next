// WSA-Next © 2026 Marek Wesołowski - wesmar
// MMS through the PC's modem. Android has no cellular network here: MmsService (patched,
// android/patches/packages/services/Mms) connects to /dev/socket/wsa_mms, and WSA-Next on Windows
// keeps a connection open on vsock port "WSMM". This daemon joins one of each and passes the
// bytes through unchanged: MmsService names the APN and the server in the first line, Windows
// opens that APN on the modem, connects and answers "OK", then the HTTP exchange follows.
//
// One exchange at a time: Windows opens its next connection when one ends, and a second
// MmsService connection waits in the listen queue meanwhile. Nothing polls: the daemon blocks
// in accept and poll until a side acts.

#define LOG_TAG "wsa-mms"

#include <cutils/sockets.h>
#include <linux/vm_sockets.h>
#include <log/log.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

namespace {

constexpr uint32_t kBridgePort = 0x57534D4D;  // "WSMM"

int ListenForHost() {
  const int s = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0) return -1;
  sockaddr_vm address{};
  address.svm_family = AF_VSOCK;
  address.svm_port = kBridgePort;
  address.svm_cid = VMADDR_CID_ANY;
  if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(s, 1) != 0) {
    close(s);
    return -1;
  }
  return s;
}

bool WriteAll(int fd, const char* data, size_t size) {
  while (size > 0) {
    const ssize_t n = write(fd, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

// Copies both ways until both directions have ended (a side's end of data is passed on as a
// half-close) or a side fails.
void Join(int a, int b) {
  const int peer[2] = {b, a};
  pollfd fds[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
  char buffer[16 * 1024];
  int open = 2;
  while (open > 0) {
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const ssize_t n = read(fds[i].fd, buffer, sizeof buffer);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) return;
      if (n == 0) {
        shutdown(peer[i], SHUT_WR);
        fds[i].fd = -1;  // poll skips it
        --open;
        continue;
      }
      if (!WriteAll(peer[i], buffer, static_cast<size_t>(n))) return;
    }
  }
}

}  // namespace

int main() {
  const int local = android_get_control_socket("wsa_mms");
  if (local < 0 || listen(local, 4) != 0) {
    ALOGE("wsa-mms: no socket from init");
    return 1;
  }
  const int listener = ListenForHost();
  if (listener < 0) {
    ALOGE("wsa-mms: cannot listen on vsock port 0x%08X (errno %d)", kBridgePort, errno);
    return 1;
  }
  for (;;) {
    const int host = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (host < 0) continue;  // EINTR, or a connection that went away before it was taken
    // Wait for MmsService. The host speaks only after it: anything from the host now is its
    // end (WSA-Next stopped), and the next connection is taken.
    pollfd fds[2] = {{host, POLLIN, 0}, {local, POLLIN, 0}};
    for (;;) {
      if (poll(fds, 2, -1) < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (fds[0].revents) break;
      if (fds[1].revents & POLLIN) {
        const int client = accept4(local, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        Join(client, host);
        close(client);
        break;
      }
    }
    close(host);
  }
}

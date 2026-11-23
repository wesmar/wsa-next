// WSA-Next © 2026 Marek Wesołowski - wesmar
// Connections from the Windows host into WSA's Android over Hyper-V sockets (vsock in the guest).
//
// Android's HALs listen on vsock ports; the bridges here connect to them as soon as WSA's VM is
// up, so nothing on either side polls while both run. Finding the VM is the only place that
// needs a timed wait: the Host Compute Service has no notification for a new compute system, so
// it is looked for every few seconds while WSA is not running; the end of the VM is a
// notification. While Android boots, its HAL may not listen yet: the connect timeout paces the
// attempts. Connecting needs no registration in the registry (only listening on the host does).
#pragma once
#include <guiddef.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace WN::Guest {

struct WsaVm {
    GUID id{};
    std::string name;
};

// The running WSA VM, if any (Host Compute Service; administrator).
std::optional<WsaVm> FindWsaVm();

// Raw summary of all compute systems from the Host Compute Service (diagnostics).
std::wstring ComputeSystemsJson();

// Keeps a connection to the guest's vsock `port` until `stop`: connects whenever WSA runs and
// runs `session(socket, stop)` for each connection, with blocking I/O that a stop request ends
// (the socket is shut down). Reconnects when the session ends while WSA keeps running.
void Serve(std::uint32_t port, const char* peerName, const std::function<void(std::uintptr_t socket, std::stop_token)>& session,
           std::stop_token stop);

// Ends WSA's VM. `request` asks Android to shut down (without waiting for it); the end of the VM
// is an HCS notification. A VM still running after `graceMs` is terminated (like a power cut).
enum class VmStop { NotRunning, ShutDown, Terminated, Failed };
VmStop StopVm(const std::function<void()>& request, unsigned graceMs);

// Removes the Hyper-V socket service registration that test builds of 2026-11-20 made for
// listening on the host (total removal of WSA-Next); true when there was one.
bool RemoveLegacyServiceRegistration(std::uint32_t port);

}  // namespace WN::Guest

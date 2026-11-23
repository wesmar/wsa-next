# Bluetooth bridge (`src/bluetooth`)

Gives Android in WSA the PC's own Bluetooth adapter - no USB dongle. Any BLE device Windows can
see works, including devices Windows has no driver for (scales, sensors, beacons, OBD-II BLE
adapters, fitness bands): WinRT exposes their advertisements and every GATT service.
Part of `WSA-Next.exe` (`WSA-Next.exe bluetooth`), free like the rest of WSA-Next.

```
Android apps -> Android Bluetooth stack (com.android.bluetooth, unchanged)
             -> HCI HAL android.hardware.bluetooth-service.wsa (android/device/wsa/wsa_x86_64/bluetooth)
             == H4 over a Hyper-V socket (the HAL listens on vsock port 0x57534254 "WSBT",
                the bridge connects when WSA's VM is up) ==
             -> Controller: an emulated Bluetooth controller
             -> WinRT: BluetoothLEAdvertisementWatcher, BluetoothLEDevice / GATT
             -> the PC's adapter
```

| Part | Files | Notes |
|---|---|---|
| Entry | `Bridge.*` | `RunBridge(stop)`: administrator check, adapter address, service registration, serve |
| Hyper-V socket client | `src/host/Guest.*` | finds the WSA VM through the Host Compute Service (its owner is WSA's localized name, which contains "Android") and connects to the HAL's vsock port as soon as Android listens; the end of the VM is an HCS notification; no registry registration is needed for connecting |
| H4 framing | `H4.*` | |
| Controller | `Controller.*`, `Hci.*`, `Bytes.h` | dual-mode Bluetooth 4.2 identity (LMP page 0 of RootCanal's CSR preset, Apache 2.0), BD_ADDR = the PC's adapter, supported-commands mask built from the implemented handlers (`Opcodes.inc`, generated from RootCanal's `hci_packets.pdl`) |
| LE scanning | `Scanner.*` | advertisements -> LE Advertising Reports (AD structures rebuilt from WinRT data sections, 31 bytes max) |
| LE links | `LeLink.*`, `L2cap.*` | LE Create Connection -> `BluetoothLEDevice` + `GattSession` (MaintainConnection); a snapshot of the GATT database with the device's handles answers discovery; reads, writes (also prepared/long), CCCD writes -> WinRT subscriptions -> notifications/indications; one worker thread per link keeps ATT in order; ACL reassembly/fragmentation and Number of Completed Packets credits; HCI Reset closes all links |
| Log | `Log.*` | `%LOCALAPPDATA%\WSA-Next\bluetooth.log` plus the console |

| Background and autostart | `src/host/Host.*` | the bridge runs in WSA-Next's background host (`WSA-Next.exe host --background`, one per session, mutex `Local\WSA-Next.Host`, events `...Host.Stop` / `...Host.Reload`) next to the Windows Hello bridge; it runs while the setting `Bluetooth` is 1; Task Scheduler task "WSA-Next" starts the host at logon with the highest privileges |

Ways to run it:

* Window: **Bluetooth › Turn on for Android** (Ctrl+B) turns the setting on and makes the host
  start the bridge; it keeps running after the window is closed; **Start with Windows** adds
  the host's logon task; the *This PC* card shows "waiting for Android" or "connected to
  Android". "Remove everything" stops the host and removes the task, the service
  registrations and the log.
* `WSA-Next.exe bluetooth`: in the console (administrator; Ctrl+C stops it).
* `WSA-Next.exe bluetooth --list-vms`: the Host Compute Service's view (diagnostics).

## Milestones

1. Bluetooth turns on in Android; scanning shows what Windows sees. *(controller and scanning
   verified with a probe client in the WSA VM: HCI init answers, LE Encrypt test vector,
   79 advertising reports in 6 s)*
2. LE connections and GATT (read, write, notifications) through `BluetoothLEDevice` /
   `GattSession`, ATT translated with WinRT's real attribute handles (`LeLink.*`, `L2cap.*`).
   *(verified with a probe client: connection to a Logitech MX Keys in 3 s, MTU exchange,
   primary services, battery level read by UUID - the real 85 % -, characteristic discovery,
   disconnection)*
3. Pairing: SMP emulated towards Android, the real pairing done by Windows.
4. Classic Bluetooth: RFCOMM / SPP through `RfcommDeviceService` (paired devices).

Windows keeps HID over GATT (0x1812) for itself: keyboards, mice and gamepads keep working
through Windows input instead. Audio stays with Windows as well.

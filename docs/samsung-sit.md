# Samsung SIT support

This tree contains a ModemManager plugin and a standalone diagnostic client for
the Samsung SIT binary protocol used by the Exynos Modem 5300 in the Pixel 9a.
The control device is normally `/dev/wwan0sit0`; the legacy downstream name
`/dev/umts_ipc0` is also accepted by the diagnostic tool.

## What is implemented

- framing, validation, token allocation, response correlation, timeouts and
  unsolicited indications, including multiple frames in one read;
- query and state-confirmed control of radio power;
- compact SIM-presence/status and packet-registration queries;
- the captured data-profile, initial-attach, OEM setup and packet-data-call
  sequences, with registration/setup retries;
- IPv4 address, DNS and MTU decoding, data-call deactivation and asynchronous
  data-call-list handling;
- carrier-manifest, device-information, capability and indication-filter
  request builders;
- a native ModemManager bearer exposing static IPv4 configuration to clients;
- `mm-sit-tool` for status, initialization, radio, connect, disconnect,
  monitoring and explicit raw requests.

SIT logs contain message names, IDs, tokens, lengths and status values. Payload
bytes are deliberately not logged because they may contain subscriber or
carrier information.

## Build and postmarketOS packaging

Enable the plugin explicitly when using a package configuration that disables
automatic features:

```sh
meson setup build --prefix=/usr -Dplugin_samsung_sit=enabled
meson compile -C build
DESTDIR="$pkgdir" meson install -C build
```

This installs the `samsung-sit` ModemManager plugin and `mm-sit-tool`. The
normal ModemManager dependencies (GLib/GIO and libgudev when udev is enabled)
are sufficient; no QMI or MBIM library is used by SIT. A local reproducible
development image used for this work is `modemmanager-dev:noble-20260710`
(Meson 1.3.2, GLib 2.80).

The kernel's WWAN `type` attribute identifies the SIT port automatically. The
plugin is restricted to the `s5300-modem` driver and expects at least one
network data port. A normal first check is:

```sh
mmcli --list-modems
mmcli --modem any
mmcli --modem any --enable
mmcli --modem any --simple-connect='apn=YOUR_APN'
```

ModemManager reports the assigned static address, DNS servers and MTU on the
bearer. A network manager normally applies those settings to the WWAN netdev.
For a minimal shell without NetworkManager, stop ModemManager and use the
diagnostic client instead:

```sh
mm-sit-tool status
mm-sit-tool radio on
mm-sit-tool --hold --configure=rmnet0 connect YOUR_APN
```

Only one process should own the SIT device. Do not run `sit-smoke` or
`mm-sit-tool` while ModemManager has the port open. In particular, closing the
owner may cause the CP to release its data call, so ModemManager intentionally
keeps the control channel open for its lifetime.

## Required services outside ModemManager

The plugin controls an already booted modem; it is not a replacement for the
rest of the modem userspace stack. A postmarketOS image still needs:

1. `cbd-lite` (or equivalent) to load the modem firmware and bring the CP to
   `ONLINE` before ModemManager probes it.
2. An RFS server holding `/dev/wwan0rfs0` open and serving the matching
   carrier-config database. The `0x4600` SIT message only selects a manifest;
   the CP fetches the actual files over RFS. Missing carrier configuration
   prevents packet registration and commonly produces data-call cause 133.
3. The `shared_modem_platform`/GEMS replacement on `oem_ipc1`, started early
   enough to answer UE-capability file requests. Merely draining this port is
   insufficient; leaving requests unanswered can crash MAIN with
   `PAL_QUEUE_FULL(LTERRC, LTEL2HRx, LteRrc)` under traffic.
4. The kernel packet-data path and netdev (PKTPROC/rmnet or its mainline WWAN
   equivalent). SIT negotiates the bearer but does not carry IP packets.

These services should be ordered so the file responders are ready before the
CP starts issuing requests, the CP reaches `ONLINE`, and only then
ModemManager is started or reprobed.

## Difference from `sit-smoke`

`sit-smoke` is an excellent early-boot proof that one request and its response
can traverse the FMT queue. The ModemManager implementation is the long-lived
control plane: it avoids a redundant radio-on request, verifies the resulting
state, correlates tokens, handles interleaved indications and partial/coalesced
frames, retries registration and PDP setup, supports disconnect, and exports a
managed bearer over D-Bus.

The standalone `mm-sit-tool` preserves the useful direct-debug workflow and
adds structured status, initialization, disconnect, monitor and raw commands.
Unlike the smoke program it uses GLib and the normal target userspace, so it is
not intended for the tiny freestanding initramfs phase.

Current known limits are IPv4-only data-call decoding, automatic rather than
manual network selection, and partial SIM decoding. Generic AT support remains
available for identity and other standard modem features when the s5300 AT
WWAN port is present. Protocol and socket-backed state-machine tests run
without hardware; final service ordering, carrier files and live data transfer
must be validated on the phone.

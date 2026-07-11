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
- automatic Samsung carrier-profile selection from `cfg.db`, including the
  captured `0x4602` SIM identity indication and `0x4600` manifest request;
- device-information, capability and indication-filter request builders;
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
normal ModemManager dependencies plus SQLite 3 are required; no QMI or MBIM
library is used by SIT. A local reproducible development image used for this
work is `modemmanager-dev:noble-20260710` (Meson 1.3.2, GLib 2.80).

## Carrier-profile selection

The plugin uses ModemManager's existing carrier-configuration lifecycle: SIM
properties are loaded first, then the SIT backend opens the Samsung `cfg.db`
read-only, resolves the best carrier ID and sends the selected 20-byte manifest
SHA-1 in `0x4600`. Bearer connection checks that this completed successfully,
so a missing database or manifest produces a clear error instead of eventually
failing PDP setup with cause 133.

The default database is
`/vendor/firmware/carrierconfig/cfg.db`. A deployment using another mount point
may set the udev property `ID_MM_SIT_CARRIER_CONFIG_DIRECTORY` on the SIT port;
the directory must contain `cfg.db`, `manifests/` and `confseqs/`, and the same
path must be visible to the RFS server.

Selection follows the vendor RIL ordering recovered from the local artifact:
an available privilege-access-rule override, then MCC/MNC plus IMSI with ranked
SPN/GID1/GID2 combinations, ICCID/IIN fallback, and finally the database's
global fallback. Manifest identifiers are validated as SHA-1 values and the
selected manifest file must exist before `0x4600` is sent. The selected symbolic
name and hash are exported as ModemManager's carrier configuration and revision.

The CP's unsolicited `0x4602` supplies ICCID and IMSI and is cached as a
fallback. The normal s5300 AT port supplies MCC/MNC, SPN and GID1/GID2 when
available, which is required to distinguish MVNO rows that share a home
network. ModemManager does not currently expose UICC carrier-privilege access
rules, so the resolver implements that highest-priority vendor path but the
live backend cannot populate it yet. Neither subscriber identifier is written
to the log.

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
mm-sit-tool registration
mm-sit-tool operator
mm-sit-tool radio on
mm-sit-tool ids SIGNAL
mm-sit-tool query GET_SIGNAL_STRENGTH
mm-sit-tool --dump-payload query GET_SIGNAL_STRENGTH
mm-sit-tool raw 0x0900 00000000
mm-sit-tool --hold --configure=rmnet0 connect YOUR_APN
```

`ids` lists the message names identified from the downstream SIT RIL and
accepts an optional case-insensitive filter. `query` and `raw` accept either a
numeric message ID or an unambiguous symbolic name. `query` supplies the usual
four-byte zero query body; `raw` sends exactly the body given by the caller.
Payloads remain redacted unless `--dump-payload` is given. The payload dump is
hexadecimal and includes response status bytes; it may expose subscriber data.

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
manual network selection, and partial SIT-native SIM decoding. Generic AT
support supplies the remaining identity and SIM-file fields when the s5300 AT
WWAN port is present. Protocol, resolver and socket-backed state-machine tests
run without hardware; final service ordering, carrier files and live data
transfer must be validated on the phone.

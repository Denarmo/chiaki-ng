# Chiaki-NG Emulator Control Protocol (ECP) Integration

This fork (`Denarmo/chiaki-ng`, branch `ecp`) adds **Emulator Control Protocol (ECP) 1, Level 2**
support for **PlayStation Remote Play (`psremote`)** on Android (`arm64-v8a`), Windows (`x86_64`),
and Linux (`x86_64`), designed for frontends such as EmuDEN.

## Target File Format (`.chiaki` / `.psremote`)

Frontends pass `.chiaki` or `.psremote` files containing a JSON object identifying a console
already registered in Chiaki-NG:

```json
{
  "nickname": "PS5-LivingRoom",
  "mac": "01:23:45:67:89:ab",
  "target": "ps5",
  "wakeup": true
}
```

Optional fields:
- `"host"`: Explicit IP/hostname when the console is on another subnet or manual-host entry.
- `"passcode"`: 4-digit PlayStation login passcode to submit automatically if prompted.
- `"wakeup"`: `true` (default) to automatically send DDP wake-up packets when in Rest Mode.

`key=value` text or a single-line MAC address, IP address, or console nickname are also accepted.

On Android, open Chiaki-NG -> menu button on any registered console -> **Save `.chiaki` for EmuDEN**.

## Android (`io.github.denarmo.chiaki`)

- `EcpPlayActivity`: Handles `io.github.ecp.action.PLAY` (ECP Level 2) and `ACTION_VIEW` for
  `.chiaki` / `.psremote` (Level 0 fallback).
  - Descriptor (`@raw/ecp`): capabilities `pause`, `menu`, `settings`, `screenshot`, `perf`,
    `volume`, `configure`.
  - Resolves registered credentials from the Chiaki Room database, discovers/wakes via
    `DiscoveryService`, displays an initial black cover until the first decoded video frame, reports
    1 Hz performance metrics via native decoded-frame count, handles `set_volume`, `PixelCopy`
    screenshots, `menu_chord` / Back pause menu hand-over, and Live/Restart ECP settings.
- `EcpConfigureReceiver`: Handles `io.github.ecp.action.CONFIGURE` broadcasts off the main thread.

## Desktop (`chiaki-ecp`)

`ecp/chiaki_ecp.c` builds a standalone C11 ECP Level 2 supervisor (`chiaki-ecp` / `chiaki-ecp.exe`)
that lives beside the upstream `chiaki.exe` or `chiaki-ng.AppImage`:
- Descriptor capabilities: `settings`, `oneshot`.
- Implements `--ecp-describe`, `--ecp-oneshot` (`describe`, `schema`, `status`, `verify`),
  `--ecp stdio`, `--ecp tcp:127.0.0.1:<port>`, and `--dry-run` (for CI/conformance probing).
- Reads Chiaki-NG's registered hosts from `HKCU\Software\Chiaki\Chiaki\registered_hosts` on
  Windows, `~/.config/Chiaki/Chiaki.conf` on Linux, or `chiaki list`.
- Sends UDP DDP search and wakeup packets directly before spawning `chiaki stream ... --exit-app-on-stream-exit`
  and supervising its lifetime (Win32 Job Object on Windows; `SIGKILL`/process teardown on Linux).

## Building Desktop `chiaki-ecp`

Windows (MinGW-w64):
```sh
gcc -std=c11 -O2 -Wall -Wextra -s ecp/chiaki_ecp.c ecp/ecp.c -lws2_32 -ladvapi32 -static -o chiaki-ecp.exe
```

Linux:
```sh
gcc -std=c11 -O2 -Wall -Wextra -s ecp/chiaki_ecp.c ecp/ecp.c -lm -lpthread -o chiaki-ecp
```

## Licensing

Chiaki-NG additions follow upstream licensing (`LicenseRef-AGPL-3.0-only-OpenSSL` / `AGPL-3.0-only`).
Bundled ECP SDK files (`ecp/ecp.c`, `ecp/ecp.h`, `android/ecp-sdk/`) remain under the MIT license
from `https://github.com/Denarmo/emulator-control-protocol`.

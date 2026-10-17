# Native Windows ARM64 Sunshine GUI (experimental)

This document describes the separate ARM64-native **desktop** Control Panel;
the existing Vue Web UI is architecture-neutral and was already shipped in
the earlier ARM64 core ZIP.

## Build provenance

- Core: `kettly1260/foundation-sunshine`, branch `experiment/native-mf-arm64`.
- GUI: pinned git submodule `qiin2333/sunshine-control-panel`, not latest
  upstream branch. The build job checks the actual git commit against the
  Foundation gitlink.
- Rust target for the Tauri frontend: `aarch64-pc-windows-msvc` (native ARM64).
- Stylus plugin target: the exact same ARM64 target and repository revision.
- Native GUI workflow: `.github/workflows/arm64-native-gui.yml`.
- CI verifies both GUI EXE and plugin DLL as PE Machine=0xAA64. The complete
  portable ZIP is likewise scanned and rejects any non-ARM64 EXE/DLL.
- No installer, service registration, signing, x64 driver or Windows security
  configuration changes are performed.

## Artifacts

1. `sunshine-gui-windows-arm64` — only GUI binaries and plugin, packaged
   as `sunshine-gui-windows-arm64.zip`.
2. `foundation-arm64-native-gui-portable-experimental` — paired Sunshine core
   and matching GUI, in a ZIP with layout:

```text
Sunshine-Foundation-Windows-ARM64/
  sunshine.exe
  assets/
    web/
    gui/
      sunshine-gui.exe
      alkaidlab-plugin-stylus.dll
  tools/
    sunshinesvc.exe (not installed)
```

The Control Panel deliberately goes under `assets/gui`, because its existing
portable-resolution logic walks up from `assets/gui/sunshine-gui.exe` to the
directory containing `sunshine.exe`.

## Initial host test

Extract to a **fresh directory**; do not overwrite the previously working
ARM64 Sunshine installation. On your Snapdragon X Elite PC:

1. Launch `sunshine.exe` from the ZIP's root in a normal user session, as
   done for previous Native MFT tests.
2. Launch `assets/gui/sunshine-gui.exe` without elevation.
3. Verify panel opens, finds the local core and shows settings/pairing.
4. Test the GUI's tray icon, show/hide, start/stop controls and native Stylus
   plugin, if used.
5. Test H.264 and HEVC Moonlight streaming with `encoder = mf_native`.
   AV1 remains deferred. Do not install any virtual display driver from this
   experimental archive.
6. Keep logs for GUI startup, core handshake and any plugin-load error.

Windows 11 ARM64 requires a functional WebView2 runtime. This workflow does not
install WebView2 and does not modify system-wide WebView2 settings.

## Update safety

The pinned upstream Control Panel currently selects
`windows-x64-installer` in its in-app updater. A native ARM64 portable build
must **not** use that path. The experimental ARM64 build will apply
`scripts/arm64_gui_compat_patch.py` to its disposable Panel checkout to
disable selection/download/execution of x64 installers for ARM64 targets.
Desktop GUI updates should only be delivered by a verified native ARM64
portable ZIP or a future correctly signed ARM64 installer.

## Acceptance

A successful CI build confirms compilability and native PE architecture, not
that Tauri, WebView2, the tray or Sunshine communication works on your device.
Do not ship as a stable release before a native host test passes.

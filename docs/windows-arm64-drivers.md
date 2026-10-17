# Windows ARM64 driver enablement (Foundation Sunshine)

Status: experimental branch only, 2026-10-17. **No ARM64 driver is shipped in the
portable installer yet.** Existing working Native MFT H.264/HEVC and Tauri GUI
are intentionally untouched.

## Read-only release audit

CI workflow: [arm64-driver-audit.yml](../.github/workflows/arm64-driver-audit.yml).
Verified all SHA256 digests for upstream pinned binary releases on a Windows
11 ARM64 runner. Evidence: GitHub Actions run 38996902972.

| Device | Pinned release | Static audit | Follow-up |
| --- | --- | --- | --- |
| Display | qiin2333/zako-vdd v0.17.5 | Released ZakoVDD.dll is x64 | Source port required |
| Mouse | AlkaidLab/zako-vmouse-release v1.3.2 | Released ZakoVirtualMouse.dll is x64, test-certificate signed | ARM64 source or independently designed driver required |
| Gamepad | nefarius/ViGEmBus v1.22.0 | Vendor-signed multi-architecture installer (x86 bootstrap); release source has native ARM64 configurations | Validate installed kernel driver and bus device |

**Do not confuse a 32-bit installer bootstrap with the architecture of its
driver.** The official ViGEmBus package installs an ARM64 ViGEmBus.sys; the
prototype CI check confirmed PE Machine=0xAA64 and valid driver signature
after installation. The first disposable-runner smoke test still failed
the PnP-present/OK assertion; a follow-up collects SCM and PnP rescan
diagnostics. Do not claim functional Xbox 360 or DS4 emulation until the
bus is present and a Sunshine/ViGEmClient target can be created.

## Display (ZakoVDD): native source port

- Exact upstream source: qiin2333/zako-vdd v0.17.5 commit
  88d117431549bcba22fea27ded3d4804c79b51fb.
- Script: [arm64_vdd_project_port.py](../scripts/arm64_vdd_project_port.py),
  cloned from Release|x64 in an isolated CI checkout (no source changes
  to the vendor repository).
- Workflow: [arm64-vdd-source-build.yml](../.github/workflows/arm64-vdd-source-build.yml).
- Build run **38997143449 succeeded**, including WDK installation, UMDF/IddCx
  DLL compilation, and PE machine check (0xAA64). ARM64 INF was stamped.
- Artifact `zako-vdd-arm64-UNSIGNED-development-only` contains DLL, INF and
  an explicit warning; intentionally lacks a signed CAT. **Not installable
  as a production release.**
- Next: obtain legitimate Windows driver-signing path, validate driver CAT,
  verify clean ARM64 PnP installation in an isolated test host, then verify
  Sunshine's existing Root\\ZakoVDD device, control IOCTL, monitor modes,
  cursor and capture frame channel, idle/sleep recovery, and removal/rollback.

Do not enable testsigning or disable Secure Boot on a daily laptop. Driver
signing and installation require an independent authorization and appropriate
Microsoft Hardware Developer Program signing/attestation or HLK workflow.

## Mouse (ZakoVirtualMouse): current blocker

The vendor's release DLL has PE machine=0x8664 (x64) and an untrusted WDK
development-signing chain on the ARM64 audit runner. Its public mirror only
hosts signed/unsigned binary releases and points to
AlkaidLab/ZakoVirtualMouse, whose source is not accessible to this project.

The **existing Sunshine mouse input remains usable via SendInput fallback**,
but this is not a real virtual HID device. It must not be mislabeled as
driver-level Raw Input.

Required next decision:
1. Obtain authorized access to source and a release-signing pipeline to port
   the same UMDF HID protocol to ARM64; OR
2. Implement a new native UMDF/VHF HID mouse plus new Sunshine bridge,
   device IDs, IOCTL/report compatibility and security review.

Never bundle the x64 UMDF DLL or trust an arbitrary WDKTestCert on ARM64.

## Gamepad (ViGEmBus): first candidate for host test

- Official release v1.22.0
  `ViGEmBus_1.22.0_x64_x86_arm64.exe`, pinned SHA256
  `89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a`.
- The upstream package has a valid Nefarius Authenticode signature on Windows
  ARM64, and the embedded native driver installed in an isolated runner is
  PE 0xAA64 with valid signature.
- Existing Foundation Sunshine already includes ViGEmClient X360/DS4
  support. No custom gamepad driver is needed for that subset if the signed
  ViGEmBus bus loads and a virtual controller can be attached.
- ViGEmBus is **end-of-life**; it does not supply Xbox Series/DualSense
  full-fidelity features. Do not advertise unsupported extra controllers.
- Test workflow: [arm64-vigembus-smoke.yml](../.github/workflows/arm64-vigembus-smoke.yml).
  Driver installation runs **only inside disposable GitHub-hosted ARM64 CI**.
  Signing, PnP state and no-reboot behavior are independently checked.
- No driver is installed on the user's PC by automation.

## Local read-only host status

On the Snapdragon laptop, optionally run from a PowerShell console:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check-arm64-driver-status.ps1
```

The script saves `sunshine-arm64-driver-status.json` and does not request
administrator elevation or change device, certificate or driver settings.

## Integration gate

Do NOT include any optional driver in the ARM64 GUI/core installer until:
1. Verified ARM64 PE and INF architecture;
2. Correct signing for an unchanged Secure Boot system;
3. Clean-host driver install/uninstall with rollback and no surprise restart;
4. Sunshine integration test (VDD 60/90 Hz monitor, mouse buttons/wheel/raw HID,
   gamepad X360/DS4);
5. Smoke test after reboot, sleep/resume and after UAC secure desktop.

The installer should keep VDD, virtual HID mouse and gamepad as separately
optional components; never mix x64 driver blobs into the ARM64 package.

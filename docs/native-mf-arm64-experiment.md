# Experimental native Media Foundation encoder (Windows ARM64)

This is a **test-only** Foundation Sunshine encoder backend on the
`experiment/native-mf-arm64` branch. It is not a replacement for the stable
FFmpeg Media Foundation path.

## Goal and scope

- H.264 / AVC Main 8-bit SDR (NV12)
- H.265 / HEVC Main 8-bit SDR (NV12)
- AV1 Main 8-bit SDR: deferred. The Qualcomm MFT enumerates, but the
  experimental AV1 path did not emit frames on the test X Elite.
- Native asynchronous Windows `IMFTransform`, using the Sunshine D3D11
  converter output and independent input texture slots
- Original capture, audio, networking, timestamps, FEC and input handling
- Existing FFmpeg encoder remains selectable, no changes to `master`

**Known target-device result:** With `encoder = mf_native`, 1080p60 H.264
reached 58.47 FPS and 18.3 ms average Moonlight-reported host processing
latency, versus >100 ms in the original FFmpeg MF path. Independent HEVC
hardware D3D11 benchmarking produced approximately 15.4 ms steady-state
latency, but a separate `mf_native` HEVC Moonlight test is still required.

**Not yet validated:** HDR/P010, YUV444, Dolby Vision, dynamic HDR metadata,
HEVC Moonlight playback, live bitrate reconfiguration, runtime encoder recovery,
and real-game latency. An earlier successful H.264 test does not qualify later
CI changes as runtime validated.

## Builds

The workflow `.github/workflows/arm64-native-mf-sunshine.yml` runs the
existing experimental ARM64 portable build plus
`scripts/arm64_native_mft_patch.py`,
`scripts/arm64_upstream_perf_patch.py` and
`scripts/arm64_lizardbyte_input_patch.py` in CI. It builds the entire Sunshine
portable package without installing a Windows service or virtual driver.

- Artifact: `foundation-arm64-NATIVE-MFT-EXPERIMENTAL-not-validated`
- Do **not** overwrite your working Sunshine folder or install drivers.
- Native builds contain an **opt-in** encoder called `mf_native`.
- To test native MFT, configure `encoder = mf_native`, restart the standalone
  Sunshine executable, and select supported codec in Moonlight.
- If it fails, revert the configuration to the previously working encoder
  (e.g., `mediafoundation`) and use the known-good Sunshine package.

## Isolated diagnostics

`.github/workflows/native-mf-arm64.yml` builds the following executables:

- `native_mft_probe.exe` — enumerates codecs and checks attributes.
- `native_mft_benchmark.exe` — frame-to-packet D3D11 H.264/HEVC benchmark.
- `native_mft_av1_benchmark.exe` — retained for diagnosis only; AV1 is
  deferred and no AV1 acceptance claim is made.

Open PowerShell in the extracted artifact directory:

```powershell
.\native_mft_benchmark.exe --codec h264 --input gpu --frames 180 2>&1 | Tee-Object h264.txt
.\native_mft_benchmark.exe --codec hevc --input gpu --frames 180 2>&1 | Tee-Object hevc.txt
```

Results save CSV files with per-frame latency and status. Use
`--output encoded.h264` / `--output encoded.hevc` / `--output encoded.obu`
for bitstream inspection when needed. The small, synthetic input scenes do not
simulate game-screen complexity, nor do benchmark durations include network or
decoder latency. A successful CI build does not prove Moonlight playback.

## Validation gates

1. CI compiles ARM64 Sunshine with opt-in native MFT.
2. The target X Elite confirms H.264 / HEVC hardware MFT availability.
3. Both required codecs emit valid decodable bitstream with keyframes and required
   codec initialization headers.
4. Moonlight displays correct video with controlled queue depth, recovery IDR,
   timestamp and transport behavior.
5. Compare mean, P95, P99 latency and dropped frames under the same scene and
   network conditions with the original FFmpeg Media Foundation path.

If any gate fails, remain on the proven working version.

## Upstream selective backports

Two independent sources are reviewed, with no blanket merges:

- `AlkaidLab/foundation-sunshine` (Foundation direct upstream):
  PRs [#1140](https://github.com/AlkaidLab/foundation-sunshine/pull/1140)
  and [#1142](https://github.com/AlkaidLab/foundation-sunshine/pull/1142)
  add source/capture/submit/output/repeat FPS counters, only collected while
  a consumer holds a lease. The experiment ports the backend API and counters.
  It does **not** yet port RTSS overlay UI or control-panel integration.
- `LizardByte/Sunshine` (official upstream):
  [#5818](https://github.com/LizardByte/Sunshine/pull/5818) serializes
  input packet processing per stream; the experimental backport preserves
  Foundation's pen validation and input activity notification.
- Already present: official
  [#4424](https://github.com/LizardByte/Sunshine/pull/4424) WGC 60 FPS cap
  fix (`MinUpdateInterval(4ms)`), so **do not reapply** it.
- Already addressed independently: official
  [#3905](https://github.com/LizardByte/Sunshine/pull/3905) ARM64 CI;
  the Foundation fork has its own portable `windows-11-arm`/CLANGARM64 build.
- Deferred: official mouse display targeting
  [#5739](https://github.com/LizardByte/Sunshine/pull/5739) and extended
  keyboard keys [#5821](https://github.com/LizardByte/Sunshine/pull/5821).
  Both depend on input backend conventions/library APIs that differ from
  Foundation. Official [#4967](https://github.com/LizardByte/Sunshine/pull/4967)
  minimum FPS policy also needs adaptation to Foundation's VRR/input-boost
  policies; do not replace Foundation frame scheduling verbatim.

Frame-rate API when the new build passes CI:

- Local-only `POST /api/perf/frame-rates` JSON:
  `{"consumer_id":"arm64-test","enabled":true,"lease_ms":30000}`
- Local-only `GET /api/perf/current` gains `frame_rates` for each session.
- Subscriber leases expire automatically; counters are inactive when unused.

CI source compilation is necessary but not sufficient. Validate mouse and
keyboard ordering, input lag, H.264/HEVC Moonlight playback and capture FPS on
the target Windows ARM64 system before considering any changes for `master`.

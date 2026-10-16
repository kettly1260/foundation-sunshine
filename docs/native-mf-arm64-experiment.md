# Experimental native Media Foundation encoder (Windows ARM64)

This is a **test-only** Foundation Sunshine encoder backend on the
`experiment/native-mf-arm64` branch. It is not a replacement for the stable
FFmpeg Media Foundation path.

## Goal and scope

- H.264 / AVC Main 8-bit SDR (NV12)
- H.265 / HEVC Main 8-bit SDR (NV12)
- AV1 Main 8-bit SDR (NV12, driver capability-dependent)
- Native asynchronous Windows `IMFTransform`, using the Sunshine D3D11
  converter output and independent input texture slots
- Original capture, audio, networking, timestamps, FEC and input handling
- Existing FFmpeg encoder remains selectable, no changes to `master`

**Not yet validated:** HDR/P010, YUV444, Dolby Vision, dynamic HDR metadata,
Moonlight decode compatibility, packet framing, live bitrate reconfiguration,
runtime encoder recovery and real-game latency.

## Builds

The workflow `.github/workflows/arm64-native-mf-sunshine.yml` runs the
existing experimental ARM64 portable build plus
`scripts/arm64_native_mft_patch.py` in CI. It builds the entire Sunshine
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
- `native_mft_av1_benchmark.exe` — AV1 hardware encoder benchmark; no fallback
  to software if Qualcomm AV1 hardware is not present.

Open PowerShell in the extracted artifact directory:

```powershell
.\native_mft_benchmark.exe --codec h264 --input gpu --frames 180 2>&1 | Tee-Object h264.txt
.\native_mft_benchmark.exe --codec hevc --input gpu --frames 180 2>&1 | Tee-Object hevc.txt
.\native_mft_av1_benchmark.exe --codec av1 --input gpu --frames 180 2>&1 | Tee-Object av1.txt
```

Results save CSV files with per-frame latency and status. Use
`--output encoded.h264` / `--output encoded.hevc` / `--output encoded.obu`
for bitstream inspection when needed. The small, synthetic input scenes do not
simulate game-screen complexity, nor do benchmark durations include network or
decoder latency. A successful CI build does not prove Moonlight playback.

## Validation gates

1. CI compiles all ARM64 tools and Sunshine with opt-in native MFT.
2. The target X Elite confirms H.264 / HEVC / AV1 hardware MFT availability.
3. Each codec emits valid decodable bitstream with keyframes and required
   codec initialization headers.
4. Moonlight displays correct video with controlled queue depth, recovery IDR,
   timestamp and transport behavior.
5. Compare mean, P95, P99 latency and dropped frames under the same scene and
   network conditions with the original FFmpeg Media Foundation path.

If any gate fails, remain on the proven working version.

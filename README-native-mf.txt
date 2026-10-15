Native Media Foundation ARM64 experiment

Purpose: explore a direct Windows MFT encoder backend separately from the
existing FFmpeg Media Foundation encoder used by Foundation Sunshine.

This is diagnostic infrastructure, not production encoder integration.
The current Foundation master branch and portable release remain unchanged.

Implementation acceptance criteria:
1. Enumerate hardware MFT encoders for H.264 and HEVC.
2. Confirm input formats, output formats, and D3D11 device-manager support.
3. Handle asynchronous IMFMediaEventGenerator NeedInput/HaveOutput events.
4. Measure queue depth and output latency, including long-tail P95/P99.
5. Preserve the current FFmpeg encoder as fallback.
6. Only introduce an opt-in encoder when isolated benchmark passes.

#!/usr/bin/env python3
"""Apply the Windows ARM64-only Native MFT prototype to Foundation at CI build time.

No committed mainline source or existing FFmpeg MFT encoder is modified.
Fail closed if upstream code has changed: no silent partial insertions.
"""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
video_path = root / "src" / "video.cpp"
cmake_path = root / "cmake" / "targets" / "common.cmake"

def replace_once(text: str, needle: str, replacement: str, path: str) -> str:
    hits = text.count(needle)
    if hits != 1:
        raise RuntimeError(f"Native MFT patch: {path} anchor {needle[:80]!r} count={hits}, expected 1")
    return text.replace(needle, replacement, 1)

code = video_path.read_text(encoding="utf-8")
if "SUNSHINE_EXPERIMENTAL_NATIVE_MFT" in code:
    raise RuntimeError("Native MFT prototype already patched; refusing to apply twice")

code = replace_once(
    code,
    '  #include "platform/windows/display.h"\n',
    '  #include "platform/windows/display.h"\n'
    '  #if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)\n'
    '    #include "platform/windows/native_mft_encoder.h"\n'
    '  #endif\n',
    "video.cpp includes",
)
code = replace_once(
    code,
    "  class avcodec_encode_session_t: public encode_session_t {",
    '  #if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)\n'
    '    #include "platform/windows/native_mft_sunshine.inc"\n'
    '  #endif\n\n'
    "  class avcodec_encode_session_t: public encode_session_t {",
    "video.cpp encoder session",
)

registry = r'''#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
  // Opt-in native Qualcomm hardware Media Foundation path.
  // Deliberately limited to 8-bit SDR, 4:2:0 in the first milestone.
  encoder_t mf_native_encoder {
    "mf_native"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12, AV_PIX_FMT_NONE, AV_PIX_FMT_NONE, AV_PIX_FMT_NONE,
      dxgi_init_avcodec_hardware_input_buffer),
    { {}, {}, {}, {}, {}, {}, "av1_mf"s },
    { {}, {}, {}, {}, {}, {}, "hevc_mf"s },
    { {}, {}, {}, {}, {}, {}, "h264_mf"s },
    PARALLEL_ENCODING
  };
#endif

'''
code = replace_once(
    code,
    '  static const std::vector<encoder_t *> encoders {',
    registry + '  static const std::vector<encoder_t *> encoders {',
    "video.cpp registry",
)
code = replace_once(
    code,
    '''#ifdef _WIN32
    &quicksync,
    &amdvce,
#endif
''',
    '''#ifdef _WIN32
    &quicksync,
    &amdvce,
  #if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
    &mf_native_encoder,
  #endif
#endif
''',
    "video.cpp registry list",
)
code = replace_once(
    code,
    '    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {',
    '''#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
    if (encoder.name == "mf_native") {
      auto converted = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      session = make_native_mft_encode_session(effective_config, std::move(converted));
    }
    else
#endif
    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {''',
    "video.cpp session factory",
)
code = replace_once(
    code,
    '    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {\n'
    '      return encode_avcodec(',
    '''#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
    if (auto native_session = dynamic_cast<native_mft_encode_session_t *>(&session)) {
      return encode_native_mft(frame_nr, *native_session, packets, channel_data,
                               frame_timestamp, std::move(pipeline_trace));
    }
    else
#endif
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      return encode_avcodec(''',
    "video.cpp encode dispatch",
)
code = replace_once(
    code,
    '''      if (collect_amf_outputs(*session, packets, channel_data)) {
        if (backend_failure_handler) backend_failure_handler();
        break;
      }

      consume_input_activity();''',
    '''      if (collect_amf_outputs(*session, packets, channel_data)) {
        if (backend_failure_handler) backend_failure_handler();
        break;
      }
#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
      if (collect_native_mft_outputs(*session, packets, channel_data)) {
        BOOST_LOG(error) << "Native MFT asynchronous output failed";
        if (backend_failure_handler) backend_failure_handler();
        break;
      }
#endif

      consume_input_activity();''',
    "video.cpp asynchronous output polling",
)

# Original capture loop knows only about AMF's asynchronous output. Without
# including Native MFT pending frames here, it may sleep for the entire next
# image interval after the hardware encoder has already produced a packet.
# On static desktops this adds substantial host latency and reduces FPS.
code = replace_once(
    code,
    '''    auto has_amf_output = [&]() {
      auto amf_session = dynamic_cast<amf_encode_session_t *>(session.get());
      return amf_session && amf_session->has_pending_output();
    };
    auto pop_image_interruptible''',
    '''    auto has_amf_output = [&]() {
      auto amf_session = dynamic_cast<amf_encode_session_t *>(session.get());
      return amf_session && amf_session->has_pending_output();
    };
    auto has_pending_async_output = [&]() {
      if (has_amf_output()) return true;
#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
      auto native_session = dynamic_cast<native_mft_encode_session_t *>(session.get());
      if (native_session && native_session->encoder) {
        return native_session->encoder->pending_count() != 0;
      }
#endif
      return false;
    };
    auto pop_image_interruptible''',
    "video.cpp Native MFT pending-output poll condition",
)
code = replace_once(
    code,
    '''      if (!has_amf_output() && (!allow_input_preemption || wait_time <= input_activity_poll_interval)) {
        return images->pop(wait_time);
      }''',
    '''      if (!has_pending_async_output() && (!allow_input_preemption || wait_time <= input_activity_poll_interval)) {
        return images->pop(wait_time);
      }''',
    "video.cpp do not block input wait while Native MFT output pending",
)
code = replace_once(
    code,
    '''        if (collect_amf_outputs(*session, packets, channel_data)) {
          amf_output_failed = true;
          return {};
        }''',
    '''        if (collect_amf_outputs(*session, packets, channel_data)) {
          amf_output_failed = true;
          return {};
        }
#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
        if (collect_native_mft_outputs(*session, packets, channel_data)) {
          BOOST_LOG(error) << "Native MFT output polling failed during capture wait";
          amf_output_failed = true;
          return {};
        }
#endif''',
    "video.cpp pump native MFT packets while waiting for next capture",
)
code = replace_once(
    code,
    '''        if (has_amf_output()) {
          slice_wait = std::min(slice_wait, amf_output_poll_interval);
        }''',
    '''        if (has_pending_async_output()) {
          // Poll pending Native MFT samples at 1 ms, same cadence as AMF.
          slice_wait = std::min(slice_wait, amf_output_poll_interval);
        }''',
    "video.cpp short-poll interval when native MFT output pending",
)
code = replace_once(
    code,
    '''    auto encoder_list = encoders;

    // If we already have a good encoder''',
    '''    auto encoder_list = encoders;
#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
    // The experimental backend is opt-in. It must not change the default
    // encoder choice until Moonlight integration has been verified.
    if (config::video.encoder != "mf_native") {
      std::erase(encoder_list, &mf_native_encoder);
    }
#endif

    // If we already have a good encoder''',
    "video.cpp opt-in filter",
)
code = replace_once(
    code,
    '    auto test_av1 = active_av1_mode >= 2 || (active_av1_mode == 0 && !(encoder.flags & H264_ONLY));',
    '    auto test_av1 = active_av1_mode >= 2 || (active_av1_mode == 0 && !(encoder.flags & H264_ONLY));\n'
    '#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)\n'
    '    if (encoder.name == "mf_native") test_av1 = false;  // AV1 frozen until valid encoded output\n'
    '#endif',
    "video.cpp skip unsupported native MFT AV1 probe",
)
code = replace_once(
    code,
    '''    auto encode_start = std::chrono::steady_clock::now();
    while (!packets->peek()) {
      if (encode(1, session, packets, nullptr, {}, {})) {''',
    '''    auto encode_start = std::chrono::steady_clock::now();
    int64_t probe_frame = 1;
    while (!packets->peek()) {
      // Asynchronous MFTs can hold startup frames. A single immutable sample
      // may never generate a packet until subsequent samples are submitted.
      const auto index =
#if defined(SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
        encoder.name == "mf_native" ? probe_frame++ :
#endif
        1;
      if (encode(index, session, packets, nullptr, {}, {})) {''',
    "video.cpp async encoder probe",
)
video_path.write_text(code, encoding="utf-8")

cmake = cmake_path.read_text(encoding="utf-8")
cmake += '''
# Local experimental Windows ARM64 Native MFT backend. Only enabled when
# scripts/arm64_native_mft_patch.py is explicitly used in the build.
if(WIN32)
  target_compile_definitions(sunshine PRIVATE SUNSHINE_EXPERIMENTAL_NATIVE_MFT)
  target_link_libraries(sunshine mfplat mfuuid mf d3d11 oleaut32 strmiids)
endif()
'''
cmake_path.write_text(cmake, encoding="utf-8")
print("Native MFT patched for experimental ARM64 build (H264/HEVC/AV1, SDR NV12 only)")

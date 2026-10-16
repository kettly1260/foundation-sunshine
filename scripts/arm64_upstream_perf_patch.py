#!/usr/bin/env python3
"""Port AlkaidLab PRs #1140/#1142 frame-rate telemetry to Foundation ARM64.
Runs after arm64_native_mft_patch.py in the isolated experiment workflow.
Files from upstream are pinned in scripts/upstream-perf rather than fetched in CI.
Any upstream anchor drift fails the build instead of silently dropping metrics.
"""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
source_dir = root / "tools" / "native-mf"

def replace_once(text: str, anchor: str, replacement: str, filename: str) -> str:
    count = text.count(anchor)
    if count != 1:
        raise RuntimeError(f"upstream-perf port: {filename}: expected one anchor, found {count}: {anchor[:115]!r}")
    return text.replace(anchor, replacement, 1)

def update(path: str, replacements):
    target = root / path
    s = target.read_text(encoding="utf-8")
    if "SUNSHINE_UPSTREAM_PERF_PORT_APPLIED" in s:
        raise RuntimeError(f"{path}: performance port already applied")
    for old, new in replacements:
        s = replace_once(s, old, new, path)
    target.write_text(s, encoding="utf-8")

def mark(text: str) -> str:
    return text + "\n// SUNSHINE_UPSTREAM_PERF_PORT_APPLIED\n"

# The original fork keeps the recorder in src/; upstream has renamed it to
# src/streaming/. Overlay the 2026-10-16 upstream implementations in the
# build directory while keeping Foundation's existing CMake source list.
for filename in ("perf_recorder.cpp", "perf_recorder.h", "frame_rate_tracker.h"):
    source = source_dir / filename
    if not source.is_file():
        raise FileNotFoundError(source)
    destination = root / "src" / filename
    if filename != "frame_rate_tracker.h" and not destination.is_file():
        raise FileNotFoundError(destination)
    destination.write_bytes(source.read_bytes())

update("src/platform/common.h", [
    (
        "struct AVDictionary;\n",
        "struct AVDictionary;\n\nnamespace perf {\n  struct capture_source_counter_t;\n}\n",
    ),
    (
        "    display_t() noexcept:",
        "    virtual std::shared_ptr<perf::capture_source_counter_t>\n"
        "    frame_rate_source() const { return {}; }\n\n"
        "    display_t() noexcept:",
    ),
])

update("src/platform/windows/display.h", [
    (
        '#include "src/platform/common.h"\n',
        '#include "src/platform/common.h"\n#include "src/frame_rate_tracker.h"\n',
    ),
    (
        "  class display_base_t: public display_t {\n  public:\n",
        "  class display_base_t: public display_t {\n  public:\n"
        "    std::shared_ptr<perf::capture_source_counter_t> capture_frame_counter =\n"
        "      std::make_shared<perf::capture_source_counter_t>();\n"
        "    std::shared_ptr<perf::capture_source_counter_t>\n"
        "    frame_rate_source() const override { return capture_frame_counter; }\n\n",
    ),
    (
        "  class duplication_t {\n  public:\n",
        "  class duplication_t {\n  public:\n"
        "    std::shared_ptr<perf::capture_source_counter_t> frame_counter;\n",
    ),
    (
        "  class wgc_capture_t {\n",
        "  class wgc_capture_t {\n"
        "    std::shared_ptr<perf::capture_source_counter_t> frame_counter;\n",
    ),
    (
        "  class vdd_capture_t {\n  public:\n",
        "  class vdd_capture_t {\n  public:\n"
        "    std::shared_ptr<perf::capture_source_counter_t> frame_rate_counter;\n",
    ),
])

update("src/platform/windows/display_base.cpp", [
    (
        "  duplication_t::init(display_base_t *display, const ::video::config_t &config) {\n    HRESULT status;\n",
        "  duplication_t::init(display_base_t *display, const ::video::config_t &config) {\n"
        "    HRESULT status;\n"
        "    frame_counter = display->frame_rate_source();\n"
        "    if (frame_counter) frame_counter->kind.store(\n"
        "      perf::capture_source_kind_e::dxgi_updates, std::memory_order_relaxed);\n",
    ),
    (
        "    auto status = dup->AcquireNextFrame(timeout.count(), &frame_info, res_p);\n",
        "    const auto epoch = perf::frame_rate_collection_epoch();\n"
        "    auto status = dup->AcquireNextFrame(timeout.count(), &frame_info, res_p);\n"
        "    if (SUCCEEDED(status) && frame_counter && frame_info.LastPresentTime.QuadPart != 0) {\n"
        "      frame_counter->record(std::max<UINT>(1, frame_info.AccumulatedFrames), epoch);\n"
        "    }\n",
    ),
])

update("src/platform/windows/display_wgc.cpp", [
    (
        "  wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {\n",
        "  wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {\n"
        "    frame_counter = display->frame_rate_source();\n"
        "    if (frame_counter) frame_counter->kind.store(\n"
        "      perf::capture_source_kind_e::wgc_arrivals, std::memory_order_relaxed);\n",
    ),
    (
        "  wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {\n",
        "  wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {\n"
        "    const auto epoch = perf::frame_rate_collection_epoch();\n",
    ),
    (
        "    if (frame != nullptr) {\n      AcquireSRWLockExclusive(&frame_lock);",
        "    if (frame != nullptr) {\n"
        "      if (frame_counter) frame_counter->record(1, epoch);\n"
        "      AcquireSRWLockExclusive(&frame_lock);",
    ),
])

update("src/platform/windows/display_vdd.cpp", [
    (
        "    // Producer released the latest slot as key 1; consumer acquires that slot.\n"
        "    SharedFrameMetadata meta_snapshot {};",
        "    // Producer released the latest slot as key 1; consumer acquires that slot.\n"
        "    const auto epoch = perf::frame_rate_collection_epoch();\n"
        "    SharedFrameMetadata meta_snapshot {};",
    ),
    (
        "    UINT32 slot = meta_snapshot.SlotIndex;\n",
        "    UINT32 slot = meta_snapshot.SlotIndex;\n"
        "    if (frame_rate_counter) frame_rate_counter->observe_sequence(\n"
        "      meta_snapshot.FrameCounter, epoch);\n",
    ),
    (
        "    monitor_idx = static_cast<unsigned int>(producer_selection.index);\n\n    for (;;) {\n",
        "    monitor_idx = static_cast<unsigned int>(producer_selection.index);\n\n"
        "    std::uint64_t attach_epoch = 0;\n"
        "    for (;;) {\n"
        "      attach_epoch = perf::frame_rate_collection_epoch();\n",
    ),
    (
        "    capture_format = dup.format();\n",
        "    capture_format = dup.format();\n"
        "    dup.frame_rate_counter = frame_rate_source();\n"
        "    if (dup.frame_rate_counter) {\n"
        "      dup.frame_rate_counter->kind.store(\n"
        "        perf::capture_source_kind_e::vdd_published, std::memory_order_relaxed);\n"
        "      dup.frame_rate_counter->observe_sequence(dup.frame_counter(), attach_epoch);\n"
        "    }\n",
    ),
])

# Expose a private (non-wire) session ID for all encoder implementations.
update("src/video.h", [
    (
        "  struct encode_session_t {\n    virtual ~encode_session_t() = default;",
        "  struct encode_session_t {\n"
        "    std::uint32_t perf_session_id = 0;\n"
        "    virtual ~encode_session_t() = default;",
    ),
])

update("src/video.cpp", [
    (
        "          for (auto &capture_ctx : capture_ctxs) {\n            capture_ctx.images->raise(captured_frame_t {",
        "          for (auto &capture_ctx : capture_ctxs) {\n"
        "            if (perf::frame_rate_collection_epoch())\n"
        "              perf::record_capture_frame(capture_ctx.config.perf_session_id, disp->frame_rate_source());\n"
        "            capture_ctx.images->raise(captured_frame_t {",
    ),
    (
        "    if (session) session->bitrate_budget = config.bitrate_budget;\n",
        "    if (session) {\n"
        "      session->bitrate_budget = config.bitrate_budget;\n"
        "      session->perf_session_id = config.perf_session_id;\n"
        "    }\n",
    ),
    (
        "      session->bitrate_budget = config.bitrate_budget;\n    }\n\n    // As a workaround for NVENC hangs",
        "      session->bitrate_budget = config.bitrate_budget;\n"
        "    }\n"
        "    session->perf_session_id = config.perf_session_id;\n\n"
        "    // As a workaround for NVENC hangs",
    ),
    (
        "      bool has_new_frame = false;\n",
        "      bool has_new_frame = false;\n"
        "      bool replayed_frame = false;\n",
    ),
    (
        "          auto &img = frame->image;\n          if (!frame->is_replay) {",
        "          auto &img = frame->image;\n"
        "          replayed_frame = frame->is_replay;\n"
        "          if (!frame->is_replay) {",
    ),
    (
        "      session->request_normal_frame();\n    }\n  }\n\n  input::touch_port_t",
        "      session->request_normal_frame();\n"
        "      perf::record_encode_submission(config.perf_session_id, !has_new_frame || replayed_frame);\n"
        "    }\n  }\n\n  input::touch_port_t",
    ),
    (
        "          if (frame_captured) {\n            pipeline_trace = img->pipeline_trace.value_or(platf::frame_pipeline_trace_t {});",
        "          if (frame_captured) {\n"
        "            if (perf::frame_rate_collection_epoch())\n"
        "              perf::record_capture_frame(ctx->config.perf_session_id, disp->frame_rate_source());\n"
        "            pipeline_trace = img->pipeline_trace.value_or(platf::frame_pipeline_trace_t {});",
    ),
    (
        "          pos->session->request_normal_frame();\n\n          ++pos;",
        "          pos->session->request_normal_frame();\n"
        "          perf::record_encode_submission(ctx->config.perf_session_id, !frame_captured);\n\n"
        "          ++pos;",
    ),
    (
        "      packet->channel_data = channel_data;\n      packets->raise(std::move(packet));",
        "      packet->channel_data = channel_data;\n"
        "      perf::record_encoded_frame(session.perf_session_id);\n"
        "      packets->raise(std::move(packet));",
    ),
    (
        "    packets->raise(std::move(packet));\n\n    return 0;\n  }\n\n  int\n  emit_amf_output(",
        "    perf::record_encoded_frame(session.perf_session_id);\n"
        "    packets->raise(std::move(packet));\n\n"
        "    return 0;\n  }\n\n  int\n  emit_amf_output(",
    ),
    (
        "    packets->raise(std::move(packet));\n\n    return 0;\n  }\n\n  int\n  collect_amf_outputs(",
        "    perf::record_encoded_frame(session.perf_session_id);\n"
        "    packets->raise(std::move(packet));\n\n"
        "    return 0;\n  }\n\n  int\n  collect_amf_outputs(",
    ),
])

# Native MFT's new packet path is not present in upstream #1140; add it
# explicitly to keep hardware MFT encoded FPS independent of queue latency.
update("src/platform/windows/native_mft_sunshine.inc", [
    (
        "    packets->raise(std::move(packet));\n  }\n  return 0;\n}",
        "    perf::record_encoded_frame(session.perf_session_id);\n"
        "    packets->raise(std::move(packet));\n  }\n  return 0;\n}",
    ),
])

http_function = r'''
  void
  setPerfFrameRates(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;
    if (!require_localhost(response, request, "controlling frame-rate collection")) return;

    try {
      if (request->content.size() > 1024) {
        throw std::invalid_argument("frame-rate subscription body must not exceed 1024 bytes");
      }
      std::stringstream content;
      content << request->content.rdbuf();
      const auto body = json::parse(content.str());
      if (!body.is_object() || !body.contains("consumer_id") || !body["consumer_id"].is_string() ||
          !body.contains("enabled") || !body["enabled"].is_boolean() ||
          (body.contains("lease_ms") && !body["lease_ms"].is_number_integer())) {
        throw std::invalid_argument("expected consumer_id (string), enabled (boolean), and optional lease_ms (integer)");
      }
      const auto lease_ms = body.value("lease_ms", std::int64_t { 30000 });
      if (lease_ms < 1000 || lease_ms > 30000) {
        throw std::invalid_argument("lease_ms must be between 1000 and 30000");
      }
      const auto state = perf::set_frame_rate_subscription(body["consumer_id"].get<std::string>(), body["enabled"].get<bool>(),
        std::chrono::milliseconds { lease_ms });
      response->write(json { { "success", true }, { "frame_rate_collection", state } }.dump(), { { "Content-Type", "application/json" } });
      response->close_connection_after_response = true;
    }
    catch (const json::exception &e) {
      write_runtime_error(response, SimpleWeb::StatusCode::client_error_bad_request, 400, e.what());
    }
    catch (const std::invalid_argument &e) {
      write_runtime_error(response, SimpleWeb::StatusCode::client_error_bad_request, 400, e.what());
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "setPerfFrameRates: " << e.what();
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, e.what());
    }
  }
'''
update("src/confighttp.cpp", [
    (
        "  void\n  changeRuntimeBitrate(resp_https_t response, req_https_t request) {",
        http_function + "\n  void\n  changeRuntimeBitrate(resp_https_t response, req_https_t request) {",
    ),
    (
        '    server.resource["^/api/perf/current$"]["GET"] = getPerfCurrent;',
        '    server.resource["^/api/perf/current$"]["GET"] = getPerfCurrent;\n'
        '    server.resource["^/api/perf/frame-rates$"]["POST"] = setPerfFrameRates;',
    ),
])

print("Ported AlkaidLab PR #1140/#1142 pipeline FPS telemetry (lease-gated) to Foundation ARM64 build")

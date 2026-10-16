// Included into src/video.cpp inside namespace video, after frame_timestamp_ring_t.
// This glue connects native MFT packets to Sunshine's normal packet/PTS/FEC pipeline.
// It is only enabled by the ARM64 experimental build patch.

class native_mft_encode_session_t final: public encode_session_t {
public:
  native_mft_encode_session_t(std::unique_ptr<platf::avcodec_encode_device_t> converter,
                              avcodec_buffer_t &&hw_device,
                              avcodec_buffer_t &&hw_frames,
                              std::unique_ptr<native_mft::encoder_t> encoder):
      device(std::move(converter)),
      hardware_device(std::move(hw_device)),
      hardware_frames(std::move(hw_frames)),
      encoder(std::move(encoder)) {}

  int convert(platf::img_t &image) override {
    return device && device->convert(image) == 0 ? 0 : -1;
  }
  void request_idr_frame() override { force_idr = true; }
  void request_normal_frame() override { force_idr = false; }
  void invalidate_ref_frames(int64_t, int64_t) override { force_idr = true; }

  bool set_bitrate(int bitrate_kbps) override {
    return encoder && bitrate_kbps > 0 && encoder->set_bitrate(static_cast<unsigned>(bitrate_kbps));
  }
  void set_dynamic_param(const dynamic_param_t &param) override {
    if (param.type == dynamic_param_type_e::BITRATE) {
      set_bitrate(param.value.int_value);
    } else {
      BOOST_LOG(debug) << "Native MFT: requested parameter requires a new encoding session";
    }
  }

  bool submit(int64_t index, bool key,
              std::vector<native_mft::encoded_frame_t> &ready, std::string &error) {
    if (!device || !device->frame || device->frame->format != AV_PIX_FMT_D3D11 ||
        !device->frame->data[0]) {
      error = "Native MFT converter did not supply a D3D11 frame";
      return false;
    }
    auto *tex = reinterpret_cast<ID3D11Texture2D *>(device->frame->data[0]);
    if (!encoder->submit(tex, index, key, ready, error)) return false;
    force_idr = false;
    return true;
  }

  bool poll(std::vector<native_mft::encoded_frame_t> &ready, std::string &error) {
    return encoder && encoder->process(ready, error);
  }

  std::unique_ptr<platf::avcodec_encode_device_t> device;
  avcodec_buffer_t hardware_device;
  avcodec_buffer_t hardware_frames;
  std::unique_ptr<native_mft::encoder_t> encoder;
  frame_timestamp_ring_t frame_timestamps;
  bool force_idr = true;
  std::optional<int64_t> last_submitted;
};

std::unique_ptr<native_mft_encode_session_t>
make_native_mft_encode_session(const config_t &config,
                               std::unique_ptr<platf::avcodec_encode_device_t> converter) {
  if (!converter || !converter->data || config.dynamicRange ||
      config.chromaSamplingType || config.videoFormat < 0 || config.videoFormat > 1) {
    BOOST_LOG(warning) << "Native MFT currently supports SDR 8-bit NV12 H.264/HEVC only (AV1 deferred)";
    return nullptr;
  }

  auto hw_device_or_error = dxgi_init_avcodec_hardware_input_buffer(converter.get());
  if (hw_device_or_error.has_right()) return nullptr;
  avcodec_buffer_t hw_device = std::move(hw_device_or_error.left());
  avcodec_buffer_t hw_frames { av_hwframe_ctx_alloc(hw_device.get()) };
  if (!hw_frames) {
    BOOST_LOG(error) << "Native MFT: av_hwframe_ctx_alloc failed";
    return nullptr;
  }
  auto *frames = reinterpret_cast<AVHWFramesContext *>(hw_frames->data);
  frames->format = AV_PIX_FMT_D3D11;
  frames->sw_format = AV_PIX_FMT_NV12;
  frames->width = config.width;
  frames->height = config.height;
  converter->init_hwframes(frames);
  if (auto init_result = av_hwframe_ctx_init(hw_frames.get())) {
    BOOST_LOG(error) << "Native MFT: FFmpeg D3D11 input frame allocation failed: " << init_result;
    return nullptr;
  }

  avcodec_frame_t frame { av_frame_alloc() };
  if (!frame) return nullptr;
  frame->format = AV_PIX_FMT_D3D11;
  frame->width = config.width;
  frame->height = config.height;
  if (converter->set_frame(frame.release(), hw_frames.get())) {
    BOOST_LOG(error) << "Native MFT: converter initialization failed";
    return nullptr;
  }
  converter->apply_colorspace();

  auto encoder = std::make_unique<native_mft::encoder_t>();
  const unsigned fps_n = config.frameRateNum > 0 ? config.frameRateNum : config.framerate;
  const unsigned fps_d = config.frameRateNum > 0 && config.frameRateDen > 0 ? config.frameRateDen : 1;
  const unsigned bitrate_kbps = config.bitrate > 0 ? config.bitrate : 10000;
  std::string error;
  if (!encoder->initialize(
        reinterpret_cast<ID3D11Device *>(converter->data),
        static_cast<native_mft::codec_e>(config.videoFormat),
        config.width, config.height, fps_n, fps_d, bitrate_kbps, error)) {
    BOOST_LOG(warning) << "Native MFT initialization failed: " << error;
    return nullptr;
  }
  BOOST_LOG(info) << "Native MFT hardware encoder active for "
                  << (config.videoFormat == 0 ? "H.264" : "HEVC");
  if (encoder->qualcomm_cfr_active()) {
    BOOST_LOG(info) << "Native MFT Qualcomm CameraRecord CFR enabled (upstream frame-drop workaround)";
  }
  return std::make_unique<native_mft_encode_session_t>(
    std::move(converter), std::move(hw_device), std::move(hw_frames), std::move(encoder));
}

int emit_native_mft_packets(
  native_mft_encode_session_t &session,
  std::vector<native_mft::encoded_frame_t> &ready,
  safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data) {
  for (auto &encoded : ready) {
    if (encoded.data.empty() || encoded.index < 0) {
      BOOST_LOG(error) << "Native MFT produced an empty or invalid packet";
      return -1;
    }
    auto packet = std::make_unique<packet_raw_generic>(
      std::move(encoded.data), encoded.index, encoded.keyframe);
    packet->channel_data = channel_data;
    packet->frame_timestamp = session.frame_timestamps.lookup(
      static_cast<uint64_t>(encoded.index));
    packet->fec_percentage = session.frame_timestamps.lookup_fec(
      static_cast<uint64_t>(encoded.index));
    auto trace = session.frame_timestamps.lookup_trace(
      static_cast<uint64_t>(encoded.index));
    if (trace) {
      trace->packet_ready = std::chrono::steady_clock::now();
      packet->pipeline_trace = std::move(trace);
    }
    packets->raise(std::move(packet));
  }
  return 0;
}

int collect_native_mft_outputs(
  encode_session_t &session,
  safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data) {
  auto *native = dynamic_cast<native_mft_encode_session_t *>(&session);
  if (!native) return 0;
  std::vector<native_mft::encoded_frame_t> ready;
  std::string status_text;
  if (!native->poll(ready, status_text)) {
    BOOST_LOG(error) << "Native MFT output failed: " << status_text;
    return -1;
  }
  if (auto diag = native->encoder->take_empty_output_diagnostics()) {
    BOOST_LOG(warning) << "Native MFT empty-output diagnostics (1s window): "
                       << "empty=" << diag->skipped_since_last
                       << ", valid=" << diag->encoded_since_last
                       << ", empty_total=" << diag->skipped_total
                       << ", valid_total=" << diag->encoded_total
                       << ", stream_flags=" << diag->output_stream_flags
                       << ", output_status=" << diag->output_status
                       << ", process_status=" << diag->process_status
                       << ", buffers=" << diag->sample_buffer_count
                       << ", sample_total_len=" << diag->sample_total_length
                       << ", max_buffer_len=" << diag->sample_buffer_capacity
                       << ", sample_flags=" << diag->sample_flags
                       << ", frame_index=" << diag->frame_index
                       << ", keyframe_requested=" << diag->requested_keyframe
                       << ", mft_owned=" << diag->output_mft_owned;
  }
  return emit_native_mft_packets(*native, ready, packets, channel_data);
}

int encode_native_mft(
  int64_t frame_nr,
  native_mft_encode_session_t &session,
  safe::mail_raw_t::queue_t<packet_t> &packets,
  void *channel_data,
  std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
  std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
  // Encoder-probing may retry the same frame until an asynchronous output
  // arrives. Do not submit duplicate indices, which would corrupt PTS matching.
  if (session.last_submitted && frame_nr <= *session.last_submitted)
    return collect_native_mft_outputs(session, packets, channel_data);

  const auto index = static_cast<uint64_t>(frame_nr);
  if (pipeline_trace) pipeline_trace->encode_submit = std::chrono::steady_clock::now();
  session.frame_timestamps.store(index, frame_timestamp, std::move(pipeline_trace),
                                 session.fec_percentage());
  std::vector<native_mft::encoded_frame_t> ready;
  std::string status_text;
  if (!session.submit(frame_nr, session.force_idr, ready, status_text)) {
    BOOST_LOG(error) << "Native MFT input failed: " << status_text;
    return -1;
  }
  session.last_submitted = frame_nr;
  if (emit_native_mft_packets(session, ready, packets, channel_data)) return -1;
  return collect_native_mft_outputs(session, packets, channel_data);
}

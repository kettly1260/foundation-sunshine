#define NOMINMAX
// Native Windows Media Foundation MFT frame-to-packet latency benchmark.
// Standalone diagnostic; DOES NOT modify or replace Sunshine.
//
// This implementation sends synthetic moving NV12 frames to a hardware-only
// encoder and matches the encoded output's PTS to the ProcessInput timestamp.
// GPU mode uses D3D11 NV12 textures (no CPU readback); CPU is an explicit
// diagnostic fallback. The 60 FPS pacing happens outside the MFT queue.
//
// Compile with MSVC ARM64: /std:c++17 /EHsc
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
constexpr size_t kTexturePool = 24;
constexpr int64_t kMFTimeUnitsPerSec = 10000000LL;

struct Options {
  bool hevc = false;
  bool av1 = true;
  bool gpu = true;
  unsigned width = 1920;
  unsigned height = 1080;
  unsigned fps = 60;
  unsigned bitrate_kbps = 10000;
  unsigned frames = 180;
  std::filesystem::path csv;
  std::filesystem::path raw;
};

std::string code(HRESULT hr) {
  std::ostringstream os;
  os << "0x" << std::uppercase << std::hex
     << static_cast<unsigned long>(hr);
  return os.str();
}

void require(HRESULT hr, const char* call) {
  if (FAILED(hr)) throw std::runtime_error(std::string(call) + ": " + code(hr));
}
void optional(HRESULT hr, const char* call) {
  std::cout << call << ": " << code(hr) << (FAILED(hr) ? " (not accepted)" : " (accepted)") << "\n";
}

bool parse(unsigned& field, const std::wstring& value, unsigned min_value, unsigned max_value) {
  try {
    size_t used = 0;
    const auto v = std::stoul(value, &used);
    if (used != value.size() || v < min_value || v > max_value) return false;
    field = static_cast<unsigned>(v);
    return true;
  } catch (...) { return false; }
}

void usage() {
  std::cout << "Native Qualcomm MFT hardware encoding latency benchmark\n"
               "  --codec h264|hevc|av1         default av1\n"
               "  --input gpu|cpu               default gpu (D3D11 NV12)\n"
               "  --width 1920 --height 1080    even dimensions only\n"
               "  --fps 60 --bitrate 10000      bitrate in kbps\n"
               "  --frames 180                  number of frames, paced at fps\n"
               "  --csv <path>                  save per-frame latency CSV\n"
               "  --output <path>               optionally write raw .h264/.hevc/.obu\n"
               "  --help                        show this message\n"
               "Example: native_mft_av1_benchmark.exe --codec av1 --frames 240\n";
}

Options parse_options(int argc, wchar_t** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::wstring key = argv[i];
    if (key == L"--help") { usage(); std::exit(0); }
    if (i + 1 >= argc) throw std::runtime_error("Missing value for a command-line argument");
    const std::wstring value = argv[++i];
    if (key == L"--codec") {
      if (value == L"h264") { o.av1=false; o.hevc=false; }
      else if (value == L"hevc") { o.av1=false; o.hevc=true; }
      else if (value == L"av1") { o.av1=true; o.hevc=false; }
      else throw std::runtime_error("Unsupported codec: use h264, hevc or av1");
    } else if (key == L"--input") {
      if (value == L"gpu") o.gpu = true;
      else if (value == L"cpu") o.gpu = false;
      else throw std::runtime_error("Unsupported input: use gpu or cpu");
    } else if (key == L"--width") {
      if (!parse(o.width,value,320,3840)) throw std::runtime_error("Invalid width");
    } else if (key == L"--height") {
      if (!parse(o.height,value,240,2160)) throw std::runtime_error("Invalid height");
    } else if (key == L"--fps") {
      if (!parse(o.fps,value,10,120)) throw std::runtime_error("Invalid fps");
    } else if (key == L"--bitrate") {
      if (!parse(o.bitrate_kbps,value,500,100000)) throw std::runtime_error("Invalid bitrate");
    } else if (key == L"--frames") {
      if (!parse(o.frames,value,30,2400)) throw std::runtime_error("Invalid frames");
    } else if (key == L"--csv") {
      o.csv = std::filesystem::path(value);
    } else if (key == L"--output") {
      o.raw = std::filesystem::path(value);
    } else {
      throw std::runtime_error("Unknown command-line argument");
    }
  }
  if ((o.width % 2) || (o.height % 2)) throw std::runtime_error("NV12 requires even width and height");
  if (o.csv.empty()) {
    o.csv = std::string("native_mft_") + (o.av1 ? "av1_" : o.hevc ? "hevc_" : "h264_") +
            (o.gpu ? "gpu.csv" : "cpu.csv");
  }
  return o;
}

void set_bool(ICodecAPI* api, const GUID& key, const char* label) {
  VARIANT value{};
  value.vt = VT_BOOL; value.boolVal = VARIANT_TRUE;
  optional(api->SetValue(&key, &value), label);
}
void set_uint32(ICodecAPI* api, const GUID& key, uint32_t n, const char* label) {
  VARIANT value{};
  value.vt = VT_UI4; value.ulVal = n;
  optional(api->SetValue(&key, &value), label);
}

struct MFT {
  ComPtr<IMFTransform> transform;
  ComPtr<IMFMediaEventGenerator> events;
  ComPtr<IMFDXGIDeviceManager> manager;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  MFT_OUTPUT_STREAM_INFO output_info{};
  DWORD input_id = 0;
  DWORD output_id = 0;
  bool provides_samples = false;
};

ComPtr<IMFMediaType> make_type(const Options& opt, bool output) {
  ComPtr<IMFMediaType> type;
  require(MFCreateMediaType(&type), "MFCreateMediaType");
  require(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF_MT_MAJOR_TYPE");
  require(type->SetGUID(MF_MT_SUBTYPE,
      output ? (opt.av1 ? MFVideoFormat_AV1 : opt.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264) : MFVideoFormat_NV12), "MF_MT_SUBTYPE");
  require(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, opt.width, opt.height), "MF_MT_FRAME_SIZE");
  require(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, opt.fps, 1), "MF_MT_FRAME_RATE");
  require(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "MF_MT_INTERLACE_MODE");
  require(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "MF_MT_PIXEL_ASPECT_RATIO");
  if (output) {
    require(type->SetUINT32(MF_MT_AVG_BITRATE, opt.bitrate_kbps * 1000), "MF_MT_AVG_BITRATE");
    if (opt.av1) {
      // AV1 Main sequence_profile is 0; vendor enum is absent in Windows SDK 26100.
      require(type->SetUINT32(MF_MT_MPEG2_PROFILE, 0), "AV1 Main profile");
    } else if (opt.hevc) {
      require(type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8), "HEVC Main profile");
    } else {
      require(type->SetUINT32(MF_MT_VIDEO_PROFILE, eAVEncH264VProfile_Main), "H.264 Main profile");
    }
  }
  return type;
}

MFT initialize_mft(const Options& opt) {
  MFT result;
  MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video,
                                   opt.av1 ? MFVideoFormat_AV1 : opt.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
  IMFActivate** enumerated = nullptr;
  UINT32 count = 0;
  require(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                    nullptr, &output, &enumerated, &count), "MFTEnumEx hardware");
  std::cout << "Hardware MFT count: " << count << "\n";
  if (!count) { CoTaskMemFree(enumerated); throw std::runtime_error("No hardware encoder; refusing software fallback"); }

  for (UINT32 i = 0; i < count; ++i) {
    wchar_t* friendly = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(enumerated[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
      std::wcout << L"MFT candidate: " << friendly << L"\n";
      CoTaskMemFree(friendly);
    }
    if (!result.transform) {
      HRESULT hr = enumerated[i]->ActivateObject(IID_PPV_ARGS(&result.transform));
      optional(hr, "ActivateObject");
      if (FAILED(hr)) result.transform.Reset();
    }
    enumerated[i]->Release();
  }
  CoTaskMemFree(enumerated);
  if (!result.transform) throw std::runtime_error("Hardware activation failed");

  ComPtr<IMFAttributes> attributes;
  require(result.transform->GetAttributes(&attributes), "GetAttributes");
  const bool async = !!MFGetAttributeUINT32(attributes.Get(), MF_TRANSFORM_ASYNC, FALSE);
  const bool d3d_aware = !!MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE);
  std::cout << "Async MFT: " << async << "; D3D11 aware: " << d3d_aware << "\n";
  if (!async) throw std::runtime_error("Benchmark requires asynchronous MFT (no synchronous fallback)");
  require(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF_TRANSFORM_ASYNC_UNLOCK");
  require(result.transform.As(&result.events), "QueryInterface IMFMediaEventGenerator");

  ComPtr<ICodecAPI> api;
  require(result.transform.As(&api), "QueryInterface ICodecAPI");
  set_bool(api.Get(), CODECAPI_AVLowLatencyMode, "CODECAPI_AVLowLatencyMode");
  set_bool(api.Get(), CODECAPI_AVEncCommonLowLatency, "CODECAPI_AVEncCommonLowLatency");
  set_uint32(api.Get(), CODECAPI_AVEncCommonRateControlMode,
             eAVEncCommonRateControlMode_CBR, "CODECAPI_AVEncCommonRateControlMode/CBR");
  set_uint32(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "CODECAPI_AVEncMPVDefaultBPictureCount");
  set_uint32(api.Get(), CODECAPI_AVEncMPVGOPSize, 120, "CODECAPI_AVEncMPVGOPSize");

  if (opt.gpu) {
    if (!d3d_aware) throw std::runtime_error("MFT not D3D11 aware; try --input cpu");
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                             nullptr, 0, D3D11_SDK_VERSION,
                             &result.device, nullptr, &result.context), "D3D11CreateDevice hardware");
    UINT token = 0;
    require(MFCreateDXGIDeviceManager(&token, &result.manager), "MFCreateDXGIDeviceManager");
    require(result.manager->ResetDevice(result.device.Get(), token), "DXGI ResetDevice");
    require(result.transform->ProcessMessage(
      MFT_MESSAGE_SET_D3D_MANAGER,
      reinterpret_cast<ULONG_PTR>(result.manager.Get())), "MFT_MESSAGE_SET_D3D_MANAGER");
  }

  // Output before input: encoder's documented negotiation order.
  auto out = make_type(opt, true);
  auto in = make_type(opt, false);
  require(result.transform->SetOutputType(0, out.Get(), 0), "SetOutputType H.264/HEVC/AV1");
  require(result.transform->SetInputType(0, in.Get(), 0), "SetInputType NV12");
  DWORD input_count=0, output_count=0;
  require(result.transform->GetStreamCount(&input_count, &output_count), "GetStreamCount");
  if (input_count != 1 || output_count != 1) throw std::runtime_error("Unsupported multi-stream MFT");
  DWORD input_id=0, output_id=0;
  HRESULT sid = result.transform->GetStreamIDs(1,&input_id,1,&output_id);
  if (sid != E_NOTIMPL) require(sid,"GetStreamIDs");
  result.input_id = input_id;
  result.output_id = output_id;
  require(result.transform->GetOutputStreamInfo(result.output_id, &result.output_info), "GetOutputStreamInfo");
  MFT_INPUT_STREAM_INFO input_info{};
  require(result.transform->GetInputStreamInfo(result.input_id, &input_info), "GetInputStreamInfo");
  result.provides_samples = !!(result.output_info.dwFlags &
                             (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
  std::cout << "Input info flags=" << input_info.dwFlags
            << " output flags=" << result.output_info.dwFlags
            << " output_buffer=" << result.output_info.cbSize
            << " provides_samples=" << result.provides_samples << "\n";
  require(result.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
          "MFT_MESSAGE_NOTIFY_BEGIN_STREAMING");
  require(result.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
          "MFT_MESSAGE_NOTIFY_START_OF_STREAM");
  return result;
}

struct PoolSlot {
  ComPtr<ID3D11Texture2D> texture;
  bool busy = false;
};
std::vector<PoolSlot> create_texture_pool(const Options& opt, const MFT& mft) {
  std::vector<PoolSlot> pool(opt.gpu ? kTexturePool : 0);
  if (!opt.gpu) return pool;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width=opt.width; desc.Height=opt.height;
  desc.MipLevels=1; desc.ArraySize=1;
  desc.Format=DXGI_FORMAT_NV12; desc.SampleDesc.Count=1;
  desc.Usage=D3D11_USAGE_DEFAULT;
  desc.BindFlags=D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
  for(auto& slot: pool)
    require(mft.device->CreateTexture2D(&desc, nullptr, &slot.texture), "CreateTexture2D NV12");
  return pool;
}

void draw_frame(std::vector<uint8_t>& pixels, unsigned width, unsigned height, unsigned index) {
  // Moving bright rectangle on changing grayscale Y, with neutral UV.
  const uint8_t base = static_cast<uint8_t>(38 + (index / 9) % 48);
  std::fill(pixels.begin(), pixels.begin() + size_t(width)*height, base);
  const unsigned box_width = std::max(16u, width/12);
  const unsigned x0 = (index*17) % (width-box_width);
  const unsigned y0 = height/3;
  for (unsigned y=y0; y<std::min(height,y0+height/4); ++y) {
    auto* row = pixels.data() + size_t(y)*width;
    std::fill(row+x0,row+x0+box_width, static_cast<uint8_t>(170+(index%60)));
  }
}

ComPtr<IMFSample> make_sample(const Options& opt, const MFT& mft,
                             std::vector<uint8_t>& pixels, PoolSlot* slot, int64_t pts) {
  ComPtr<IMFSample> sample;
  require(MFCreateSample(&sample), "MFCreateSample");
  ComPtr<IMFMediaBuffer> buffer;
  const DWORD length = static_cast<DWORD>(pixels.size());
  if (opt.gpu) {
    if (!slot) throw std::runtime_error("No free D3D11 texture");
    mft.context->UpdateSubresource(slot->texture.Get(),0,nullptr,pixels.data(),opt.width,length);
    require(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                     slot->texture.Get(),0,FALSE,&buffer),
            "MFCreateDXGISurfaceBuffer");
    // Surface buffers may compute their own size. Set length only if supported.
    ComPtr<IMF2DBuffer> two_d;
    if(SUCCEEDED(buffer.As(&two_d))) {
      DWORD contiguous=0;
      if(SUCCEEDED(two_d->GetContiguousLength(&contiguous)))
        optional(buffer->SetCurrentLength(contiguous),"DXGI buffer length");
    }
  } else {
    require(MFCreateMemoryBuffer(length,&buffer), "MFCreateMemoryBuffer");
    BYTE* destination=nullptr;
    require(buffer->Lock(&destination,nullptr,nullptr),"IMFMediaBuffer::Lock");
    std::memcpy(destination,pixels.data(),length);
    require(buffer->Unlock(),"IMFMediaBuffer::Unlock");
    require(buffer->SetCurrentLength(length),"IMFMediaBuffer::SetCurrentLength");
  }
  require(sample->AddBuffer(buffer.Get()),"IMFSample::AddBuffer");
  require(sample->SetSampleTime(pts),"IMFSample::SetSampleTime");
  require(sample->SetSampleDuration(kMFTimeUnitsPerSec/opt.fps),"IMFSample::SetSampleDuration");
  return sample;
}

struct Pending {
  Clock::time_point submitted;
  unsigned frame_index=0;
  size_t texture_slot=0;
};
struct Row {
  unsigned index=0;
  int64_t pts=0;
  double latency_ms=0;
  DWORD bytes=0;
  bool keyframe=false;
};
struct State {
  std::map<int64_t,Pending> pending;
  std::vector<Row> rows;
  unsigned submitted=0;
  unsigned completed=0;
  unsigned request_tokens=0;
  unsigned peak_inflight=0;
  uint64_t total_bytes=0;
  bool drain_complete=false;
  bool output_file_active=false;
  std::ofstream raw;
};

void output_one(MFT& mft, std::vector<PoolSlot>& pool, State& state) {
  MFT_OUTPUT_DATA_BUFFER out{};
  out.dwStreamID = mft.output_id;
  DWORD status=0;
  ComPtr<IMFSample> allocated;
  if (!mft.provides_samples) {
    require(MFCreateSample(&allocated),"MFCreateSample output");
    ComPtr<IMFMediaBuffer> buffer;
    const DWORD cap = std::max<DWORD>(mft.output_info.cbSize, 2*1024*1024);
    require(MFCreateMemoryBuffer(cap,&buffer),"MFCreateMemoryBuffer output");
    require(allocated->AddBuffer(buffer.Get()),"AddBuffer output");
    out.pSample = allocated.Get();
  }
  const HRESULT hr = mft.transform->ProcessOutput(0,1,&out,&status);
  if (out.pEvents) out.pEvents->Release();
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    throw std::runtime_error("Encoder output stream changed; renegotiation required");
  require(hr,"IMFTransform::ProcessOutput");

  ComPtr<IMFSample> sample;
  if (allocated) sample = allocated;
  else sample.Attach(out.pSample);
  if (!sample) throw std::runtime_error("Encoder returned no output sample");
  LONGLONG pts=0;
  require(sample->GetSampleTime(&pts),"encoded sample PTS");
  const auto match = state.pending.find(pts);
  if (match == state.pending.end())
    throw std::runtime_error("Encoded PTS has no matching input: " + std::to_string(pts));
  const double latency=std::chrono::duration<double,std::milli>(
                          Clock::now() - match->second.submitted).count();
  const unsigned frame_index=match->second.frame_index;
  const size_t slot_id=match->second.texture_slot;
  ComPtr<IMFMediaBuffer> contiguous;
  require(sample->ConvertToContiguousBuffer(&contiguous),"ConvertToContiguousBuffer output");
  DWORD bytes=0;
  require(contiguous->GetCurrentLength(&bytes),"GetCurrentLength output");
  if (!bytes) throw std::runtime_error("Encoded sample is empty");
  if (state.output_file_active) {
    BYTE* data=nullptr;
    require(contiguous->Lock(&data,nullptr,nullptr),"Lock bitstream");
    state.raw.write(reinterpret_cast<const char*>(data),bytes);
    require(contiguous->Unlock(),"Unlock bitstream");
    if (!state.raw) throw std::runtime_error("Unable to write encoded output");
  }
  const bool keyframe = MFGetAttributeUINT32(sample.Get(),MFSampleExtension_CleanPoint,FALSE)!=0;
  state.rows.push_back({frame_index,pts,latency,bytes,keyframe});
  state.total_bytes+=bytes;
  state.completed++;
  if (!pool.empty()) pool.at(slot_id).busy=false;
  state.pending.erase(match);
}

void pump_events(MFT& mft, std::vector<PoolSlot>& pool, State& state) {
  // Bounded poll: never block forever on a driver event.
  for (int i=0;i<128;i++) {
    ComPtr<IMFMediaEvent> event;
    HRESULT hr = mft.events->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) return;
    require(hr,"IMFMediaEventGenerator::GetEvent");
    MediaEventType type=MEUnknown;
    HRESULT event_status=S_OK;
    require(event->GetType(&type),"GetType");
    require(event->GetStatus(&event_status),"GetStatus");
    require(event_status,"MFT event status");
    if (type==METransformNeedInput) {
      state.request_tokens++;
      if (state.request_tokens>256)
        throw std::runtime_error("Unexpected excessive NeedInput events");
    } else if (type==METransformHaveOutput) {
      output_one(mft,pool,state);
    } else if (type==METransformDrainComplete) {
      state.drain_complete=true;
    } else if(type==MEError) {
      throw std::runtime_error("MFT reported MEError");
    }
  }
}

double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(),values.end());
  const size_t i=static_cast<size_t>(std::ceil(fraction*values.size()))-1;
  return values.at(std::min(i,values.size()-1));
}

void summarize(const Options& options, State& state, Clock::time_point start, Clock::time_point finish) {
  std::sort(state.rows.begin(),state.rows.end(),[](const Row&a,const Row&b){return a.index<b.index;});
  std::ofstream csv(options.csv);
  if (!csv) throw std::runtime_error("Could not open CSV output file");
  csv << "frame,pts_100ns,submit_to_packet_ms,encoded_bytes,keyframe\n";
  std::vector<double> ms;
  for(const auto& r:state.rows) {
    csv << r.index << "," << r.pts << "," << std::fixed << std::setprecision(3)
        << r.latency_ms << "," << r.bytes << "," << (r.keyframe?1:0) << "\n";
    ms.push_back(r.latency_ms);
  }
  csv.close();
  std::cout << "\n=== Actual hardware MFT encode results ===\n"
            << "codec=" << (options.av1?"AV1":options.hevc?"HEVC":"H264")
            << " input=" << (options.gpu?"D3D11-NV12":"system-memory-NV12")
            << " resolution=" << options.width << "x" << options.height
            << " fps_target=" << options.fps << " bitrate_kbps=" << options.bitrate_kbps << "\n"
            << "submitted=" << state.submitted << " completed=" << state.completed
            << " peak_inflight=" << state.peak_inflight
            << " encoded_bytes=" << state.total_bytes << "\n";
  if (!ms.empty()) {
    auto mm=std::minmax_element(ms.begin(),ms.end());
    const double mean=std::accumulate(ms.begin(),ms.end(),0.0)/ms.size();
    std::cout << std::fixed << std::setprecision(2)
              << "submit_to_packet_ms min=" << *mm.first << " mean=" << mean
              << " p50=" << percentile(ms,.50) << " p95=" << percentile(ms,.95)
              << " p99=" << percentile(ms,.99) << " max=" << *mm.second << "\n";
  }
  const double secs=std::chrono::duration<double>(finish-start).count();
  std::cout << "elapsed_s=" << std::fixed << std::setprecision(2) << secs
            << " avg_packet_fps=" << state.completed/std::max(0.001,secs)
            << " csv=" << options.csv.string() << "\n";
  if (state.completed!=options.frames)
    std::cout << "INCOMPLETE: output count differs from input count; do NOT treat as passing.\n";
}

int benchmark(const Options& opt) {
  std::cout << "Hardware-only benchmark; no Sunshine or driver installation.\n";
  std::cout << "codec=" << (opt.av1?"AV1":opt.hevc?"HEVC":"H264")
            << " input=" << (opt.gpu?"GPU":"CPU")
            << " frame_count=" << opt.frames << "\n";
  MFT mft = initialize_mft(opt);
  auto pool = create_texture_pool(opt,mft);
  std::vector<uint8_t> pixels(size_t(opt.width)*opt.height*3/2,128);
  State state;
  if (!opt.raw.empty()) {
    state.raw.open(opt.raw,std::ios::binary);
    if (!state.raw) throw std::runtime_error("Cannot open raw bitstream output");
    state.output_file_active=true;
  }
  const auto begin=Clock::now();
  const auto duration = std::chrono::milliseconds(
    (static_cast<uint64_t>(opt.frames)*1000)/opt.fps+15000);
  bool sent_drain=false;
  while(state.completed<opt.frames) {
    const auto now=Clock::now();
    if (now-begin>duration) {
      std::cout << "TIMEOUT waiting for hardware encoder; submitted=" << state.submitted
                << " completed=" << state.completed << " queued=" << state.pending.size() << "\n";
      break;
    }
    pump_events(mft,pool,state);
    if (state.submitted < opt.frames && state.request_tokens &&
        state.pending.size() < kTexturePool) {
      const auto due=begin+std::chrono::microseconds(
        (static_cast<uint64_t>(state.submitted)*1000000)/opt.fps);
      if (Clock::now() >= due) {
        size_t slot_index=0;
        bool free_slot=true;
        if (opt.gpu) {
          free_slot=false;
          for(size_t i=0;i<pool.size();i++) {
            if (!pool[i].busy) {slot_index=i;free_slot=true;break;}
          }
        }
        if (free_slot) {
          const unsigned index=state.submitted;
          const int64_t pts=static_cast<int64_t>(index)*kMFTimeUnitsPerSec/opt.fps;
          draw_frame(pixels,opt.width,opt.height,index);
          auto sample=make_sample(opt,mft,pixels,opt.gpu?&pool[slot_index]:nullptr,pts);
          const auto stamp=Clock::now();
          HRESULT hr=mft.transform->ProcessInput(mft.input_id,sample.Get(),0);
          require(hr,"IMFTransform::ProcessInput");
          state.pending.emplace(pts,Pending{stamp,index,slot_index});
          if (opt.gpu) pool[slot_index].busy=true;
          state.submitted++;
          state.request_tokens--;
          state.peak_inflight=std::max<unsigned>(state.peak_inflight,
                                                static_cast<unsigned>(state.pending.size()));
        }
      }
    }
    if (state.submitted==opt.frames && !sent_drain) {
      require(mft.transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0),"MFT_MESSAGE_COMMAND_DRAIN");
      sent_drain=true;
    }
    if (state.completed<opt.frames) std::this_thread::sleep_for(1ms);
  }
  const auto finish=Clock::now();
  optional(mft.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,0),"MFT_END_OF_STREAM");
  optional(mft.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH,0),"MFT_FLUSH");
  summarize(opt,state,begin,finish);
  return state.completed==opt.frames ? 0 : 4;
}

} // namespace

int wmain(int argc,wchar_t** argv) {
  try {
    const Options options=parse_options(argc,argv);
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    require(hr,"CoInitializeEx");
    hr=MFStartup(MF_VERSION);
    if(FAILED(hr)){CoUninitialize();require(hr,"MFStartup");}
    int status=1;
    try {status=benchmark(options);}
    catch(const std::exception& e) { std::cerr << "BENCHMARK FAILED: " << e.what() << "\n"; }
    require(MFShutdown(),"MFShutdown");
    CoUninitialize();
    return status;
  } catch(const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}

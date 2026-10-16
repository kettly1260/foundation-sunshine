// Experimental Native Media Foundation hardware encoder for Windows ARM64.
// Copyright: original implementation for Foundation Sunshine experiments.
// Standalone from FFmpeg encoding; Foundation may still use FFmpeg's D3D11 frame pool
// and its existing GPU color converter. All methods are called on the encoder thread.
// Supported wire formats are capability-gated at run time (H.264/HEVC/AV1).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <codecapi.h>
#include <strmif.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Some MinGW/Windows SDK combinations define function-like min/max macros
// before this header is included by Sunshine.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace video::native_mft {
using Microsoft::WRL::ComPtr;
using steady_clock = std::chrono::steady_clock;

// Only SDR 8-bit NV12 is offered in the first integration. HDR/P010,
// YUV444 and dynamic metadata must be validated independently before enabling.
enum class codec_e : int { h264 = 0, hevc = 1, av1 = 2 };

struct encoded_frame_t {
  std::vector<std::uint8_t> data;
  std::int64_t index = -1;
  bool keyframe = false;
};

class encoder_t {
public:
  encoder_t() = default;
  encoder_t(const encoder_t &) = delete;
  encoder_t &operator=(const encoder_t &) = delete;
  ~encoder_t() {
    if (transform_) {
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    transform_.Reset();
    events_.Reset();
    codec_api_.Reset();
    manager_.Reset();
    context_.Reset();
    if (mf_started_) MFShutdown();
    if (co_initialized_) CoUninitialize();
  }

  static const GUID &codec_guid(codec_e codec) {
    switch (codec) {
      case codec_e::h264: return MFVideoFormat_H264;
      case codec_e::hevc: return MFVideoFormat_HEVC;
      case codec_e::av1: return MFVideoFormat_AV1;
    }
    return MFVideoFormat_H264;
  }

  bool initialize(ID3D11Device *device, codec_e codec, unsigned width,
                  unsigned height, unsigned fps_n, unsigned fps_d,
                  unsigned bitrate_kbps, std::string &reason) {
    try {
      if (!device || !width || !height || !fps_n || !fps_d || !bitrate_kbps ||
          (width & 1) || (height & 1)) {
        throw std::runtime_error("invalid D3D11 or NV12 stream parameters");
      }
      codec_ = codec;
      width_ = width;
      height_ = height;
      fps_n_ = fps_n;
      fps_d_ = fps_d;
      const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      if (FAILED(cohr) && cohr != RPC_E_CHANGED_MODE)
        check(cohr, "CoInitializeEx");
      co_initialized_ = SUCCEEDED(cohr);
      check(MFStartup(MF_VERSION), "MFStartup");
      mf_started_ = true;

      IMFActivate **found = nullptr;
      UINT32 count = 0;
      MFT_REGISTER_TYPE_INFO encoded_type {MFMediaType_Video, codec_guid(codec)};
      check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
          MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
          nullptr, &encoded_type, &found, &count), "MFTEnumEx");
      if (!count) {
        CoTaskMemFree(found);
        throw std::runtime_error("hardware MFT unavailable (no software fallback in native backend)");
      }
      for (UINT32 i = 0; i < count; ++i) {
        if (!transform_) {
          HRESULT hr = found[i]->ActivateObject(IID_PPV_ARGS(&transform_));
          if (FAILED(hr)) transform_.Reset();
        }
        found[i]->Release();
      }
      CoTaskMemFree(found);
      if (!transform_) throw std::runtime_error("MFT hardware activation failed");

      ComPtr<IMFAttributes> attr;
      check(transform_->GetAttributes(&attr), "MFT GetAttributes");
      if (!MFGetAttributeUINT32(attr.Get(), MF_TRANSFORM_ASYNC, FALSE))
        throw std::runtime_error("synchronous MFT unsupported by native path");
      if (!MFGetAttributeUINT32(attr.Get(), MF_SA_D3D11_AWARE, FALSE))
        throw std::runtime_error("MFT is not D3D11 aware");
      check(attr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MFT async unlock");
      check(transform_.As(&events_), "MFT event generator");
      check(transform_.As(&codec_api_), "MFT ICodecAPI");

      set_bool(CODECAPI_AVLowLatencyMode);
      set_bool(CODECAPI_AVEncCommonLowLatency);
      set_u32(CODECAPI_AVEncCommonRateControlMode,
              static_cast<UINT32>(eAVEncCommonRateControlMode_CBR));
      set_u32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
      set_u32(CODECAPI_AVEncMPVGOPSize, std::max(30u, fps_n / fps_d * 2));
      set_bitrate(bitrate_kbps);

      device->GetImmediateContext(&context_);
      if (!context_) throw std::runtime_error("D3D11 immediate context missing");
      UINT reset_token = 0;
      check(MFCreateDXGIDeviceManager(&reset_token, &manager_), "MFCreateDXGIDeviceManager");
      check(manager_->ResetDevice(device, reset_token), "DXGI manager ResetDevice");
      check(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(manager_.Get())), "MFT set D3D manager");

      // Media Foundation hardware encoders negotiate output before input.
      const auto make_type = [&](bool output) {
        ComPtr<IMFMediaType> type;
        check(MFCreateMediaType(&type), "MFCreateMediaType");
        check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF_MT_MAJOR_TYPE");
        check(type->SetGUID(MF_MT_SUBTYPE,
              output ? codec_guid(codec) : MFVideoFormat_NV12), "MF_MT_SUBTYPE");
        check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height), "MF_MT_FRAME_SIZE");
        check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_n, fps_d), "MF_MT_FRAME_RATE");
        check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "InterlaceMode");
        check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "PixelAspectRatio");
        if (output) {
          check(type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_kbps * 1000), "AvgBitrate");
          if (codec == codec_e::h264) {
            // Windows MF declares VIDEO_PROFILE as an alias of MPEG2_PROFILE.
            // MinGW exposes only the original name.
            check(type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main), "H264 Main");
          } else if (codec == codec_e::hevc) {
            check(type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8), "HEVC Main");
          } else {
            // AV1 Main is profile 0. The Windows 11 26100 SDK lacks
            // eAVEncAV1VProfile_Main (present in some vendor SDKs).
            check(type->SetUINT32(MF_MT_MPEG2_PROFILE, 0), "AV1 Main");
          }
        }
        return type;
      };
      auto output_type = make_type(true);
      auto input_type = make_type(false);
      check(transform_->SetOutputType(0, output_type.Get(), 0), "MFT SetOutputType");
      check(transform_->SetInputType(0, input_type.Get(), 0), "MFT SetInputType");
      DWORD inputs = 0, outputs = 0;
      check(transform_->GetStreamCount(&inputs, &outputs), "MFT GetStreamCount");
      if (inputs != 1 || outputs != 1)
        throw std::runtime_error("MFT requires multiple streams");
      const HRESULT ids = transform_->GetStreamIDs(1, &input_id_, 1, &output_id_);
      if (ids != E_NOTIMPL) check(ids, "MFT GetStreamIDs");
      check(transform_->GetOutputStreamInfo(output_id_, &output_info_), "MFT GetOutputStreamInfo");
      output_provided_ = !!(output_info_.dwFlags &
           (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
      check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MFT begin streaming");
      check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MFT start stream");

      D3D11_TEXTURE2D_DESC desc {};
      desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
      desc.Format=DXGI_FORMAT_NV12; desc.SampleDesc.Count=1;
      desc.Usage=D3D11_USAGE_DEFAULT;
      desc.BindFlags=D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
      for (auto &slot : slots_) {
        check(device->CreateTexture2D(&desc,nullptr,&slot.texture), "Native MF input texture");
      }
      return true;
    } catch (const std::exception &e) {
      reason = e.what();
      return false;
    }
  }

  bool set_bitrate(unsigned bitrate_kbps) {
    if (!codec_api_ || bitrate_kbps == 0) return false;
    return set_u32(CODECAPI_AVEncCommonMeanBitRate, bitrate_kbps * 1000);
  }

  bool process(std::vector<encoded_frame_t> &ready, std::string &error) {
    try {
      // Do not block waiting for driver work unless a new frame must be sent.
      return drain_events(ready);
    } catch (const std::exception &e) {
      error = e.what();
      return false;
    }
  }

  bool submit(ID3D11Texture2D *source, std::int64_t frame_index, bool idr,
              std::vector<encoded_frame_t> &ready, std::string &error) {
    try {
      if (!source) throw std::runtime_error("null D3D11 converted texture");
      D3D11_TEXTURE2D_DESC input_desc {};
      source->GetDesc(&input_desc);
      if (input_desc.Width != width_ || input_desc.Height != height_ ||
          input_desc.Format != DXGI_FORMAT_NV12)
        throw std::runtime_error("converted texture is not NV12 at negotiated dimensions");

      // A bounded queue is important: the asynchronous transform may buffer many frames.
      // Never overwrite an input texture that the hardware MFT may still reference.
      const auto deadline = steady_clock::now() + std::chrono::milliseconds(150);
      size_t slot_index = slots_.size();
      for (;;) {
        drain_events(ready);
        if (input_requests_ && pending_.size() < slots_.size()) {
          for (size_t i=0; i<slots_.size(); ++i) {
            if (!slots_[i].busy) { slot_index=i; break; }
          }
          if (slot_index != slots_.size()) break;
        }
        if (steady_clock::now() > deadline)
          throw std::runtime_error("MFT input backpressure exceeded 150 ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      if (idr && codec_api_) {
        VARIANT v{}; v.vt=VT_UI4; v.ulVal=1;
        // Some drivers reject force-keyframe; that is NOT a valid stream
        // for a Moonlight recovery request. Reject instead of lying.
        check(codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v),
              "MFT force keyframe");
      }

      auto &slot = slots_[slot_index];
      context_->CopyResource(slot.texture.Get(), source);
      ComPtr<IMFSample> sample;
      ComPtr<IMFMediaBuffer> buf;
      check(MFCreateSample(&sample), "MFCreateSample input");
      check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                     slot.texture.Get(), 0, FALSE, &buf),
            "MFCreateDXGISurfaceBuffer");
      ComPtr<IMF2DBuffer> two_d;
      if (SUCCEEDED(buf.As(&two_d))) {
        DWORD size=0;
        if (SUCCEEDED(two_d->GetContiguousLength(&size)))
          check(buf->SetCurrentLength(size),"MFT input buffer length");
      }
      check(sample->AddBuffer(buf.Get()), "AddBuffer");
      const auto pts = (frame_index * 10000000LL * fps_d_) / fps_n_;
      check(sample->SetSampleTime(pts), "SetSampleTime");
      check(sample->SetSampleDuration((10000000LL * fps_d_) / fps_n_), "SetSampleDuration");
      check(transform_->ProcessInput(input_id_,sample.Get(),0), "MFT ProcessInput");
      slot.busy = true;
      pending_.emplace(pts, pending_t {frame_index,slot_index});
      --input_requests_;
      return true;
    } catch (const std::exception &e) {
      error = e.what();
      return false;
    }
  }

  size_t pending_count() const { return pending_.size(); }

private:
  struct slot_t {
    ComPtr<ID3D11Texture2D> texture;
    bool busy = false;
  };
  struct pending_t {
    std::int64_t index;
    size_t slot;
  };

  static void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) {
      char buffer[100];
      std::snprintf(buffer,sizeof(buffer),"%s returned 0x%08lX",operation,
                    static_cast<unsigned long>(hr));
      throw std::runtime_error(buffer);
    }
  }
  bool set_u32(const GUID &guid, UINT32 x) {
    VARIANT v{}; v.vt=VT_UI4; v.ulVal=x;
    return SUCCEEDED(codec_api_->SetValue(&guid,&v));
  }
  bool set_bool(const GUID &guid) {
    VARIANT v{}; v.vt=VT_BOOL; v.boolVal=VARIANT_TRUE;
    return SUCCEEDED(codec_api_->SetValue(&guid,&v));
  }

  bool drain_events(std::vector<encoded_frame_t> &ready) {
    for (int i=0;i<128;++i) {
      ComPtr<IMFMediaEvent> ev;
      const HRESULT hr=events_->GetEvent(MF_EVENT_FLAG_NO_WAIT,&ev);
      if (hr==MF_E_NO_EVENTS_AVAILABLE) return true;
      check(hr,"MFT GetEvent");
      MediaEventType type=MEUnknown;
      HRESULT status=S_OK;
      check(ev->GetType(&type),"MFT GetType");
      check(ev->GetStatus(&status),"MFT GetStatus");
      check(status,"MFT event");
      if (type==METransformNeedInput) {
        ++input_requests_;
        if (input_requests_ > 256) throw std::runtime_error("invalid excess input requests");
      } else if (type==METransformHaveOutput) {
        read_output(ready);
      } else if (type==MEError) {
        throw std::runtime_error("MFT asynchronous MEError");
      }
    }
    return true;
  }

  void read_output(std::vector<encoded_frame_t> &ready) {
    MFT_OUTPUT_DATA_BUFFER out {};
    out.dwStreamID=output_id_;
    DWORD status=0;
    ComPtr<IMFSample> sample_allocated;
    if (!output_provided_) {
      check(MFCreateSample(&sample_allocated),"MFCreateSample output");
      ComPtr<IMFMediaBuffer> buf;
      check(MFCreateMemoryBuffer(std::max<DWORD>(output_info_.cbSize,2*1024*1024),&buf),
            "MFCreateMemoryBuffer output");
      check(sample_allocated->AddBuffer(buf.Get()),"AddBuffer output");
      out.pSample=sample_allocated.Get();
    }
    const HRESULT hr=transform_->ProcessOutput(0,1,&out,&status);
    if (out.pEvents) out.pEvents->Release();
    if (hr==MF_E_TRANSFORM_NEED_MORE_INPUT) return;
    if (hr==MF_E_TRANSFORM_STREAM_CHANGE)
      throw std::runtime_error("MFT output stream changed; reinitialization required");
    check(hr,"MFT ProcessOutput");
    ComPtr<IMFSample> sample;
    if (sample_allocated) sample=sample_allocated;
    else sample.Attach(out.pSample);
    if (!sample) throw std::runtime_error("MFT returned no output sample");
    LONGLONG pts=0;
    check(sample->GetSampleTime(&pts),"MFT output timestamp");
    auto it=pending_.find(pts);
    if (it==pending_.end()) throw std::runtime_error("MFT output timestamp does not match input");
    ComPtr<IMFMediaBuffer> contiguous;
    check(sample->ConvertToContiguousBuffer(&contiguous),"MFT encoded buffer");
    DWORD length=0;
    check(contiguous->GetCurrentLength(&length),"MFT encoded buffer length");
    if (length==0 || length>16*1024*1024)
      throw std::runtime_error("MFT encoded output empty or too large");
    BYTE *data=nullptr;
    check(contiguous->Lock(&data,nullptr,nullptr),"MFT encoded buffer lock");
    encoded_frame_t packet;
    packet.index=it->second.index;
    packet.keyframe = MFGetAttributeUINT32(sample.Get(),MFSampleExtension_CleanPoint,FALSE)!=0;
    packet.data.assign(data,data+length);
    contiguous->Unlock();
    slots_[it->second.slot].busy=false;
    pending_.erase(it);
    ready.push_back(std::move(packet));
  }

  bool co_initialized_=false;
  bool mf_started_=false;
  codec_e codec_=codec_e::h264;
  unsigned width_=0,height_=0,fps_n_=0,fps_d_=1;
  ComPtr<IMFTransform> transform_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> codec_api_;
  ComPtr<IMFDXGIDeviceManager> manager_;
  ComPtr<ID3D11DeviceContext> context_;
  MFT_OUTPUT_STREAM_INFO output_info_ {};
  bool output_provided_=false;
  DWORD input_id_=0,output_id_=0;
  unsigned input_requests_=0;
  std::array<slot_t,12> slots_ {};
  std::map<LONGLONG,pending_t> pending_;
};

} // namespace video::native_mft

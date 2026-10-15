// Direct Windows Media Foundation hardware encoder probe (ARM64).
// Diagnostic only; no Sunshine runtime modifications.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <iostream>
#include <string>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
using Microsoft::WRL::ComPtr;
static void report(const char* name, HRESULT hr) {
  std::cout << name << ": 0x" << std::hex << static_cast<unsigned>(hr) << std::dec << "\n";
}
static void apply(ICodecAPI* api, const GUID& property, const char* name) {
  VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
  report(name, api->SetValue(&property, &v));
  VariantClear(&v);
}
static int probe(const GUID& codec, const char* name) {
  MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, codec};
  IMFActivate** found = nullptr;
  UINT32 count = 0;
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
      nullptr, &out, &found, &count);
  report("MFTEnumEx", hr);
  if (FAILED(hr)) return 2;
  std::cout << name << " hardware encoder count: " << count << "\n";
  if (!count) { CoTaskMemFree(found); return 3; }
  ComPtr<IMFTransform> encoder;
  for (UINT32 i = 0; i < count; i++) {
    wchar_t* friendly = nullptr; UINT32 length = 0;
    if (SUCCEEDED(found[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
        &friendly, &length))) {
      std::wcout << L"MFT: " << friendly << L"\n";
      CoTaskMemFree(friendly);
    }
    if (!encoder) {
      hr = found[i]->ActivateObject(IID_PPV_ARGS(&encoder));
      report("ActivateObject", hr);
      if (FAILED(hr)) encoder.Reset();
    }
    found[i]->Release();
  }
  CoTaskMemFree(found);
  if (!encoder) return 4;
  ComPtr<ICodecAPI> api;
  hr = encoder.As(&api);
  report("ICodecAPI", hr);
  if (SUCCEEDED(hr)) {
    apply(api.Get(), CODECAPI_AVLowLatencyMode, "AVLowLatencyMode (VT_BOOL)");
    apply(api.Get(), CODECAPI_AVEncCommonLowLatency, "AVEncCommonLowLatency (VT_BOOL)");
  }
  ComPtr<IMFMediaEventGenerator> events;
  hr = encoder.As(&events);
  std::cout << "Asynchronous event generator available: "
            << (SUCCEEDED(hr) ? "yes" : "no") << "\n";
  MFT_INPUT_STREAM_INFO inInfo = {};
  hr = encoder->GetInputStreamInfo(0, &inInfo);
  report("GetInputStreamInfo", hr);
  if (SUCCEEDED(hr))
    std::cout << "input stream flags: " << inInfo.dwFlags
              << " required alignment: " << inInfo.cbAlignment << "\n";
  MFT_OUTPUT_STREAM_INFO outInfo = {};
  hr = encoder->GetOutputStreamInfo(0, &outInfo);
  report("GetOutputStreamInfo", hr);
  if (SUCCEEDED(hr))
    std::cout << "output stream flags: " << outInfo.dwFlags
              << " minimum buffer: " << outInfo.cbSize << "\n";
  std::cout << "NOTE: no fabricated encode latency. Real GPU input and event-driven"
            << " MFT streaming are required for benchmark acceptance.\n";
  return 0;
}
int wmain() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  report("CoInitializeEx", hr);
  if (FAILED(hr)) return 1;
  hr = MFStartup(MF_VERSION);
  report("MFStartup", hr);
  if (FAILED(hr)) { CoUninitialize(); return 1; }
  const int h264 = probe(MFVideoFormat_H264, "H.264");
  const int hevc = probe(MFVideoFormat_HEVC, "HEVC");
  MFShutdown();
  CoUninitialize();
  return h264 ? h264 : hevc;
}

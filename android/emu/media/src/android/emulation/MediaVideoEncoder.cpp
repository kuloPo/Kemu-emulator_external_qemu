// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#include "host-common/MediaVideoEncoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "host-common/GoldfishVideoEncoderDefs.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "nvenc/nvEncodeAPI.h"
#endif

namespace android {
namespace emulation {
using namespace goldfish_encoder;

struct MediaVideoEncoder::Impl {
  std::mutex mutex;
#ifdef _WIN32
  using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
  using GetVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
  HMODULE library = nullptr;
  NV_ENCODE_API_FUNCTION_LIST api = {};
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  void* encoder = nullptr;
  NV_ENC_REGISTERED_PTR registered = nullptr;
  NV_ENC_OUTPUT_PTR bitstream = nullptr;
  NV_ENC_CONFIG config = {};
  NV_ENC_INITIALIZE_PARAMS init = {};
  Request settings = {};
  MediaCodecType codec = MediaCodecType::Max;
  uint32_t textureFormat = UINT32_MAX;
  bool initialized = false;
  bool headersPending = false;
  std::vector<uint8_t> headers;

  ~Impl() { close(); }

  void close() {
    if (encoder) {
      if (registered) api.nvEncUnregisterResource(encoder, registered);
      if (bitstream) api.nvEncDestroyBitstreamBuffer(encoder, bitstream);
      api.nvEncDestroyEncoder(encoder);
    }
    registered = nullptr;
    bitstream = nullptr;
    encoder = nullptr;
    texture.Reset();
    context.Reset();
    device.Reset();
    if (library) FreeLibrary(library);
    library = nullptr;
    initialized = false;
    textureFormat = UINT32_MAX;
    headers.clear();
  }

  bool check(NVENCSTATUS status, const char* operation) {
    if (status == NV_ENC_SUCCESS) return true;
    const char* detail = encoder && api.nvEncGetLastErrorString
                             ? api.nvEncGetLastErrorString(encoder)
                             : "";
    fprintf(stderr, "Kemu NVENC: %s failed (%d): %s\n", operation,
            static_cast<int>(status), detail ? detail : "");
    return false;
  }

  bool open() {
    library = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr,
                             LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return false;
    auto create = reinterpret_cast<CreateInstance>(
        GetProcAddress(library, "NvEncodeAPICreateInstance"));
    auto getVersion = reinterpret_cast<GetVersion>(
        GetProcAddress(library, "NvEncodeAPIGetMaxSupportedVersion"));
    uint32_t version = 0;
    if (!create || !getVersion || getVersion(&version) != NV_ENC_SUCCESS ||
        version < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION))
      return false;
    api = {};
    api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create(&api) != NV_ENC_SUCCESS) return false;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    for (UINT i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc = {};
      if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de ||
          (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        continue;
      if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                   nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, nullptr, &context)))
        continue;
      NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = {};
      openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
      openParams.apiVersion = NVENCAPI_VERSION;
      openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
      openParams.device = device.Get();
      if (api.nvEncOpenEncodeSessionEx(&openParams, &encoder) == NV_ENC_SUCCESS)
        return true;
      encoder = nullptr;
      context.Reset();
      device.Reset();
    }
    return false;
  }

  static bool valid(const Request& r) {
    return r.version == kVersion && r.width >= 160 && r.width <= 2560 &&
           r.height >= 64 && r.height <= 2560 && !(r.width & 1) &&
           !(r.height & 1) && r.bitrate >= 64000 && r.bitrate <= 100000000 &&
           r.frameRate >= 1000 && r.frameRate <= 60000 && r.format <= kRgba &&
           r.profile <= 2 && r.bitrateMode <= 1 && r.fullRange <= 1 &&
           r.primaries <= 255 && r.transfer <= 255 && r.matrix <= 255;
  }

  bool start(MediaCodecType type, const Request& r) {
    close();
    if (!open()) return false;
    codec = type;
    const bool hevc = type == MediaCodecType::HevcEncoder;
    GUID guid = hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    NV_ENC_CAPS_PARAM caps = {};
    caps.version = NV_ENC_CAPS_PARAM_VER;
    int minWidth = 0, minHeight = 0;
    caps.capsToQuery = NV_ENC_CAPS_WIDTH_MIN;
    api.nvEncGetEncodeCaps(encoder, guid, &caps, &minWidth);
    caps.capsToQuery = NV_ENC_CAPS_HEIGHT_MIN;
    api.nvEncGetEncodeCaps(encoder, guid, &caps, &minHeight);
    if (r.width < static_cast<uint32_t>(minWidth) ||
        r.height < static_cast<uint32_t>(minHeight)) {
      fprintf(stderr, "Kemu NVENC: minimum input size is %dx%d\n", minWidth,
              minHeight);
      return false;
    }
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (!check(api.nvEncGetEncodePresetConfigEx(
                   encoder, guid, NV_ENC_PRESET_P3_GUID,
                   NV_ENC_TUNING_INFO_LOW_LATENCY, &preset),
               "get preset"))
      return false;
    config = preset.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.profileGUID = hevc             ? NV_ENC_HEVC_PROFILE_MAIN_GUID
                         : r.profile == 2 ? NV_ENC_H264_PROFILE_HIGH_GUID
                         : r.profile == 1 ? NV_ENC_H264_PROFILE_MAIN_GUID
                                          : NV_ENC_H264_PROFILE_BASELINE_GUID;
    config.gopLength = r.gop ? r.gop : NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;
    config.rcParams.rateControlMode =
        r.bitrateMode ? NV_ENC_PARAMS_RC_VBR : NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = r.bitrate;
    config.rcParams.maxBitRate = r.bitrate;
    config.rcParams.vbvBufferSize = r.bitrate / 4;
    config.rcParams.vbvInitialDelay = r.bitrate / 4;
    config.rcParams.enableLookahead = 0;
    config.rcParams.lookaheadDepth = 0;
    config.rcParams.zeroReorderDelay = 1;
    auto setVui = [&r](NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui) {
      vui.videoSignalTypePresentFlag = 1;
      vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
      vui.videoFullRangeFlag = r.fullRange;
      vui.colourDescriptionPresentFlag = 1;
      vui.colourPrimaries =
          static_cast<NV_ENC_VUI_COLOR_PRIMARIES>(r.primaries);
      vui.transferCharacteristics =
          static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(r.transfer);
      vui.colourMatrix = static_cast<NV_ENC_VUI_MATRIX_COEFFS>(r.matrix);
    };
    if (hevc) {
      auto& c = config.encodeCodecConfig.hevcConfig;
      c.chromaFormatIDC = 1;
      c.inputBitDepth = c.outputBitDepth = NV_ENC_BIT_DEPTH_8;
      c.idrPeriod = config.gopLength;
      c.repeatSPSPPS = 1;
      c.level = r.level;
      setVui(c.hevcVUIParameters);
    } else {
      auto& c = config.encodeCodecConfig.h264Config;
      c.chromaFormatIDC = 1;
      c.inputBitDepth = c.outputBitDepth = NV_ENC_BIT_DEPTH_8;
      c.idrPeriod = config.gopLength;
      c.repeatSPSPPS = 1;
      c.level = r.level;
      setVui(c.h264VUIParameters);
    }
    init = {};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = guid;
    init.presetGUID = NV_ENC_PRESET_P3_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeWidth = init.darWidth = init.maxEncodeWidth = r.width;
    init.encodeHeight = init.darHeight = init.maxEncodeHeight = r.height;
    init.frameRateNum = r.frameRate;
    init.frameRateDen = 1000;
    init.enablePTD = 1;
    init.enableEncodeAsync = 0;
    init.encodeConfig = &config;
    if (!check(api.nvEncInitializeEncoder(encoder, &init), "initialize"))
      return false;
    initialized = true;
    NV_ENC_CREATE_BITSTREAM_BUFFER output = {};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (!check(api.nvEncCreateBitstreamBuffer(encoder, &output),
               "create bitstream"))
      return false;
    bitstream = output.bitstreamBuffer;
    headers.resize(4096);
    uint32_t headerSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD seq = {};
    seq.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    seq.inBufferSize = static_cast<uint32_t>(headers.size());
    seq.spsppsBuffer = headers.data();
    seq.outSPSPPSPayloadSize = &headerSize;
    if (!check(api.nvEncGetSequenceParams(encoder, &seq),
               "get sequence headers") ||
        headerSize > headers.size() || !headerSize)
      return false;
    headers.resize(headerSize);
    headersPending = true;
    settings = r;
    fprintf(stderr, "Kemu NVENC: %s %ux%u initialized (minimum %dx%d)\n",
            hevc ? "HEVC" : "AVC", r.width, r.height, minWidth, minHeight);
    return true;
  }

  bool prepareTexture(uint32_t format) {
    if (texture && textureFormat == format) return true;
    if (registered) {
      if (!check(api.nvEncUnregisterResource(encoder, registered),
                 "unregister input"))
        return false;
      registered = nullptr;
    }
    texture.Reset();
    DXGI_FORMAT dxgi =
        format == kRgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_NV12;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = settings.width;
    desc.Height = settings.height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = dxgi;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return false;
    NV_ENC_REGISTER_RESOURCE resource = {};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.width = settings.width;
    resource.height = settings.height;
    resource.resourceToRegister = texture.Get();
    resource.bufferFormat =
        format == kRgba ? NV_ENC_BUFFER_FORMAT_ABGR : NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (!check(api.nvEncRegisterResource(encoder, &resource), "register input"))
      return false;
    registered = resource.registeredResource;
    textureFormat = format;
    return true;
  }

  bool reconfigure(const Request& r) {
    if (r.bitrate == settings.bitrate && r.frameRate == settings.frameRate)
      return true;
    NV_ENC_CONFIG updated = config;
    updated.rcParams.averageBitRate = updated.rcParams.maxBitRate = r.bitrate;
    updated.rcParams.vbvBufferSize = updated.rcParams.vbvInitialDelay =
        r.bitrate / 4;
    NV_ENC_RECONFIGURE_PARAMS change = {};
    change.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    change.reInitEncodeParams = init;
    change.reInitEncodeParams.encodeConfig = &updated;
    change.reInitEncodeParams.frameRateNum = r.frameRate;
    if (!check(api.nvEncReconfigureEncoder(encoder, &change), "reconfigure"))
      return false;
    config = updated;
    init.frameRateNum = r.frameRate;
    settings.bitrate = r.bitrate;
    settings.frameRate = r.frameRate;
    return true;
  }

  bool encode(Request& r, uint8_t* memory) {
    if (!prepareTexture(r.format) || !reconfigure(r)) return false;
    if (headersPending)
      fprintf(stderr, "Kemu NVENC: input=%s bitrate=%u fps=%.3f\n",
              r.format == kRgba ? "RGBA" : "NV12", r.bitrate,
              r.frameRate / 1000.0);
    UINT pitch = r.width * (r.format == kRgba ? 4 : 1);
    context->UpdateSubresource(texture.Get(), 0, nullptr, memory + kInputOffset,
                               pitch, 0);
    NV_ENC_MAP_INPUT_RESOURCE mapped = {};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = registered;
    if (!check(api.nvEncMapInputResource(encoder, &mapped), "map input"))
      return false;
    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = mapped.mappedResource;
    pic.bufferFmt = mapped.mappedBufferFmt;
    pic.inputWidth = r.width;
    pic.inputHeight = r.height;
    pic.inputTimeStamp = r.timestamp;
    pic.outputBitstream = bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if ((r.flags & kForceIdr) || headersPending)
      pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    bool ok = check(api.nvEncEncodePicture(encoder, &pic), "encode picture");
    if (ok) {
      NV_ENC_LOCK_BITSTREAM output = {};
      output.version = NV_ENC_LOCK_BITSTREAM_VER;
      output.outputBitstream = bitstream;
      ok = check(api.nvEncLockBitstream(encoder, &output), "lock bitstream");
      if (ok) {
        const size_t prefix = headersPending ? headers.size() : 0;
        ok = output.bitstreamSizeInBytes <= kBufferCapacity - prefix;
        if (ok) {
          auto* dst = memory + kOutputOffset;
          if (prefix) memcpy(dst, headers.data(), prefix);
          memcpy(dst + prefix, output.bitstreamBufferPtr,
                 output.bitstreamSizeInBytes);
          r.configSize = static_cast<uint32_t>(prefix);
          r.outputSize = output.bitstreamSizeInBytes;
          r.keyFrame = output.pictureType == NV_ENC_PIC_TYPE_IDR ||
                       output.pictureType == NV_ENC_PIC_TYPE_I;
          r.timestamp = output.outputTimeStamp;
          headersPending = false;
        }
        ok = check(api.nvEncUnlockBitstream(encoder, bitstream),
                   "unlock bitstream") &&
             ok;
      }
    }
    return check(api.nvEncUnmapInputResource(encoder, mapped.mappedResource),
                 "unmap input") &&
           ok;
  }
#endif
};

MediaVideoEncoder::MediaVideoEncoder() : mImpl(new Impl()) {}
MediaVideoEncoder::~MediaVideoEncoder() = default;

uint32_t MediaVideoEncoder::supportedCodecs() {
#ifdef _WIN32
  const char* enabled = std::getenv("KEMU_HW_ENCODER");
  if (enabled && !strcmp(enabled, "0")) return 0;
  static const uint32_t supported = [] {
    uint32_t result = 0;
    Request r = {};
    r.version = kVersion;
    r.width = 320;
    r.height = 240;
    r.bitrate = 4000000;
    r.frameRate = 30000;
    r.gop = 30;
    r.primaries = r.transfer = r.matrix = 2;
    for (auto type :
         {MediaCodecType::H264Encoder, MediaCodecType::HevcEncoder}) {
      Impl probe;
      if (probe.start(type, r))
        result |= type == MediaCodecType::H264Encoder ? 1 : 2;
    }
    return result;
  }();
  return supported;
#else
  return 0;
#endif
}

void MediaVideoEncoder::handlePing(MediaCodecType type, MediaOperation op,
                                   void* memory) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  // Copy untrusted guest fields once; do not validate and then reread them.
  Request r;
  memcpy(&r, memory, sizeof(r));
  r.status = kUnavailable;
  r.outputSize = r.configSize = r.keyFrame = 0;
#ifdef _WIN32
  auto& impl = *mImpl;
  if (op == MediaOperation::DestroyContext || op == MediaOperation::Flush ||
      op == MediaOperation::Reset) {
    impl.close();
    r.status = kOk;
  } else if (!Impl::valid(r)) {
    r.status = kBadRequest;
  } else if (op == MediaOperation::InitContext ||
             op == MediaOperation::EncodeImage) {
    const uint64_t pixels = uint64_t(r.width) * r.height;
    const uint64_t expected = r.format == kRgba ? pixels * 4 : pixels * 3 / 2;
    if (op == MediaOperation::EncodeImage &&
        (r.inputSize != expected || expected > kBufferCapacity)) {
      r.status = kBadRequest;
    } else {
      const bool restart = op == MediaOperation::InitContext ||
                           !impl.initialized || type != impl.codec ||
                           r.width != impl.settings.width ||
                           r.height != impl.settings.height ||
                           r.profile != impl.settings.profile ||
                           r.level != impl.settings.level ||
                           r.bitrateMode != impl.settings.bitrateMode ||
                           r.gop != impl.settings.gop ||
                           r.fullRange != impl.settings.fullRange ||
                           r.primaries != impl.settings.primaries ||
                           r.transfer != impl.settings.transfer ||
                           r.matrix != impl.settings.matrix;
      if (!restart || impl.start(type, r)) {
        r.status = op != MediaOperation::EncodeImage ||
                           impl.encode(r, static_cast<uint8_t*>(memory))
                       ? kOk
                       : kEncodeFailed;
      }
      if (r.status != kOk) impl.close();
    }
  } else {
    r.status = kBadRequest;
  }
#else
  (void)type;
  (void)op;
#endif
  memcpy(memory, &r, sizeof(r));
}
}  // namespace emulation
}  // namespace android

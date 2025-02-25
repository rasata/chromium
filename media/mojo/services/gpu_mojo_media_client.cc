// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/mojo/services/gpu_mojo_media_client.h"

#include <utility>

#include "base/bind.h"
#include "base/feature_list.h"
#include "base/memory/ptr_util.h"
#include "gpu/ipc/service/gpu_channel.h"
#include "media/base/audio_decoder.h"
#include "media/base/cdm_factory.h"
#include "media/base/fallback_video_decoder.h"
#include "media/base/media_switches.h"
#include "media/base/video_decoder.h"
#include "media/gpu/buildflags.h"
#include "media/gpu/gpu_video_accelerator_util.h"
#include "media/gpu/gpu_video_decode_accelerator_factory.h"
#include "media/gpu/ipc/service/media_gpu_channel_manager.h"
#include "media/gpu/ipc/service/vda_video_decoder.h"
#include "media/mojo/interfaces/video_decoder.mojom.h"
#include "media/video/supported_video_decoder_config.h"
#include "media/video/video_decode_accelerator.h"

#if defined(OS_ANDROID)
#include "base/memory/ptr_util.h"
#include "media/base/android/android_cdm_factory.h"
#include "media/filters/android/media_codec_audio_decoder.h"
#include "media/gpu/android/android_video_surface_chooser_impl.h"
#include "media/gpu/android/codec_allocator.h"
#include "media/gpu/android/media_codec_video_decoder.h"
#include "media/gpu/android/video_frame_factory_impl.h"
#include "media/mojo/interfaces/media_drm_storage.mojom.h"
#include "media/mojo/interfaces/provision_fetcher.mojom.h"
#include "media/mojo/services/mojo_media_drm_storage.h"
#include "media/mojo/services/mojo_provision_fetcher.h"
#include "services/service_manager/public/cpp/connect.h"
#endif  // defined(OS_ANDROID)

#if defined(OS_WIN)
#include "media/gpu/windows/d3d11_video_decoder.h"
#include "ui/gl/gl_angle_util_win.h"
#endif  // defined(OS_WIN)

#if defined(OS_ANDROID)
#include "media/mojo/services/android_mojo_util.h"
using media::android_mojo_util::CreateProvisionFetcher;
using media::android_mojo_util::CreateMediaDrmStorage;
#endif  // defined(OS_ANDROID)

namespace media {

namespace {

#if defined(OS_ANDROID) || defined(OS_CHROMEOS) || defined(OS_MACOSX) || \
    defined(OS_WIN) || defined(OS_LINUX)
gpu::CommandBufferStub* GetCommandBufferStub(
    base::WeakPtr<MediaGpuChannelManager> media_gpu_channel_manager,
    base::UnguessableToken channel_token,
    int32_t route_id) {
  if (!media_gpu_channel_manager)
    return nullptr;

  gpu::GpuChannel* channel =
      media_gpu_channel_manager->LookupChannel(channel_token);
  if (!channel)
    return nullptr;

  gpu::CommandBufferStub* stub = channel->LookupCommandBuffer(route_id);
  if (!stub)
    return nullptr;

  // Only allow stubs that have a ContextGroup, that is, the GLES2 ones. Later
  // code assumes the ContextGroup is valid.
  if (!stub->decoder_context()->GetContextGroup())
    return nullptr;

  return stub;
}
#endif

#if defined(OS_WIN)
// Return a callback to get the D3D11 device for D3D11VideoDecoder.  Since it
// only supports the ANGLE device right now, that's what we return.
D3D11VideoDecoder::GetD3D11DeviceCB GetD3D11DeviceCallback() {
  return base::BindRepeating(
      []() { return gl::QueryD3D11DeviceObjectFromANGLE(); });
}
#endif

}  // namespace

GpuMojoMediaClient::GpuMojoMediaClient(
    const gpu::GpuPreferences& gpu_preferences,
    const gpu::GpuDriverBugWorkarounds& gpu_workarounds,
    const gpu::GpuFeatureInfo& gpu_feature_info,
    scoped_refptr<base::SingleThreadTaskRunner> gpu_task_runner,
    base::WeakPtr<MediaGpuChannelManager> media_gpu_channel_manager,
    AndroidOverlayMojoFactoryCB android_overlay_factory_cb,
    CdmProxyFactoryCB cdm_proxy_factory_cb)
    : gpu_preferences_(gpu_preferences),
      gpu_workarounds_(gpu_workarounds),
      gpu_feature_info_(gpu_feature_info),
      gpu_task_runner_(std::move(gpu_task_runner)),
      media_gpu_channel_manager_(std::move(media_gpu_channel_manager)),
      android_overlay_factory_cb_(std::move(android_overlay_factory_cb)),
      cdm_proxy_factory_cb_(std::move(cdm_proxy_factory_cb)) {}

GpuMojoMediaClient::~GpuMojoMediaClient() = default;

void GpuMojoMediaClient::Initialize(service_manager::Connector* connector) {}

std::unique_ptr<AudioDecoder> GpuMojoMediaClient::CreateAudioDecoder(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
#if defined(OS_ANDROID)
  return std::make_unique<MediaCodecAudioDecoder>(task_runner);
#else
  return nullptr;
#endif  // defined(OS_ANDROID)
}

std::vector<SupportedVideoDecoderConfig>
GpuMojoMediaClient::GetSupportedVideoDecoderConfigs() {
#if defined(OS_ANDROID)
  static std::vector<SupportedVideoDecoderConfig> supported_configs =
      MediaCodecVideoDecoder::GetSupportedConfigs();
  return supported_configs;
#else
  std::vector<SupportedVideoDecoderConfig> supported_configs;

#if defined(OS_WIN)
  // Start with the configurations supported by D3D11VideoDecoder.
  // VdaVideoDecoder is still used as a fallback.
  if (!d3d11_supported_configs_) {
    d3d11_supported_configs_ =
        D3D11VideoDecoder::GetSupportedVideoDecoderConfigs(
            gpu_preferences_, gpu_workarounds_, GetD3D11DeviceCallback());
  }
  supported_configs = *d3d11_supported_configs_;
#endif

  // VdaVideoDecoder will be used to wrap a VDA. Add the configs supported
  // by the VDA implementation.
  // TODO(sandersd): Move conversion code into VdaVideoDecoder.
  VideoDecodeAccelerator::Capabilities capabilities =
      GpuVideoAcceleratorUtil::ConvertGpuToMediaDecodeCapabilities(
          GpuVideoDecodeAcceleratorFactory::GetDecoderCapabilities(
              gpu_preferences_, gpu_workarounds_));
  bool allow_encrypted =
      capabilities.flags &
      VideoDecodeAccelerator::Capabilities::SUPPORTS_ENCRYPTED_STREAMS;
  for (const auto& supported_profile : capabilities.supported_profiles) {
    supported_configs.push_back(SupportedVideoDecoderConfig(
        supported_profile.profile,           // profile_min
        supported_profile.profile,           // profile_max
        supported_profile.min_resolution,    // coded_size_min
        supported_profile.max_resolution,    // coded_size_max
        allow_encrypted,                     // allow_encrypted
        supported_profile.encrypted_only));  // require_encrypted
  }

  return supported_configs;
#endif  // defined(OS_ANDROID)
}

std::unique_ptr<VideoDecoder> GpuMojoMediaClient::CreateVideoDecoder(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    MediaLog* media_log,
    mojom::CommandBufferIdPtr command_buffer_id,
    RequestOverlayInfoCB request_overlay_info_cb,
    const gfx::ColorSpace& target_color_space,
    mojom::VideoDecoderImplementation implementation) {
  // All implementations require a command buffer.
  if (!command_buffer_id)
    return nullptr;

  // TODO(liberato): Support non-default for D3D11VideoDecoder.
  if (implementation != mojom::VideoDecoderImplementation::Default)
    return nullptr;

#if defined(OS_ANDROID)
  auto get_stub_cb =
      base::Bind(&GetCommandBufferStub, media_gpu_channel_manager_,
                 command_buffer_id->channel_token, command_buffer_id->route_id);
  return std::make_unique<MediaCodecVideoDecoder>(
      gpu_preferences_, gpu_feature_info_, DeviceInfo::GetInstance(),
      CodecAllocator::GetInstance(gpu_task_runner_),
      std::make_unique<AndroidVideoSurfaceChooserImpl>(
          DeviceInfo::GetInstance()->IsSetOutputSurfaceSupported()),
      android_overlay_factory_cb_, std::move(request_overlay_info_cb),
      std::make_unique<VideoFrameFactoryImpl>(gpu_task_runner_,
                                              std::move(get_stub_cb)));
#elif defined(OS_CHROMEOS) || defined(OS_MACOSX) || defined(OS_WIN) || \
    defined(OS_LINUX)
  std::unique_ptr<VideoDecoder> vda_video_decoder = VdaVideoDecoder::Create(
      task_runner, gpu_task_runner_, media_log->Clone(), target_color_space,
      gpu_preferences_, gpu_workarounds_,
      base::BindRepeating(&GetCommandBufferStub, media_gpu_channel_manager_,
                          command_buffer_id->channel_token,
                          command_buffer_id->route_id));
#if defined(OS_WIN)
  if (base::FeatureList::IsEnabled(kD3D11VideoDecoder)) {
    // If nothing has cached the configs yet, then do so now.
    if (!d3d11_supported_configs_)
      GetSupportedVideoDecoderConfigs();

    std::unique_ptr<VideoDecoder> d3d11_video_decoder =
        D3D11VideoDecoder::Create(
            gpu_task_runner_, media_log->Clone(), gpu_preferences_,
            gpu_workarounds_,
            base::BindRepeating(
                &GetCommandBufferStub, media_gpu_channel_manager_,
                command_buffer_id->channel_token, command_buffer_id->route_id),
            GetD3D11DeviceCallback(), *d3d11_supported_configs_);
    return base::WrapUnique<VideoDecoder>(new FallbackVideoDecoder(
        std::move(d3d11_video_decoder), std::move(vda_video_decoder)));
  }
#endif  // defined(OS_WIN)
  return vda_video_decoder;
#else
  return nullptr;
#endif  // defined(OS_ANDROID)
}

std::unique_ptr<CdmFactory> GpuMojoMediaClient::CreateCdmFactory(
    service_manager::mojom::InterfaceProvider* interface_provider) {
#if defined(OS_ANDROID)
  return std::make_unique<AndroidCdmFactory>(
      base::Bind(&CreateProvisionFetcher, interface_provider),
      base::Bind(&CreateMediaDrmStorage, interface_provider));
#else
  return nullptr;
#endif  // defined(OS_ANDROID)
}

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
std::unique_ptr<CdmProxy> GpuMojoMediaClient::CreateCdmProxy(
    const base::Token& cdm_guid) {
  if (cdm_proxy_factory_cb_)
    return cdm_proxy_factory_cb_.Run(cdm_guid);

  return nullptr;
}
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)

}  // namespace media

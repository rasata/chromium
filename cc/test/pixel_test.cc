// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/test/pixel_test.h"

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/memory/shared_memory.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "cc/base/switches.h"
#include "cc/raster/raster_buffer_provider.h"
#include "cc/test/fake_output_surface_client.h"
#include "cc/test/pixel_test_output_surface.h"
#include "cc/test/pixel_test_utils.h"
#include "cc/test/test_in_process_context_provider.h"
#include "components/viz/client/client_resource_provider.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "components/viz/common/frame_sinks/copy_output_request.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/viz/common/quads/compositor_frame_metadata.h"
#include "components/viz/common/resources/bitmap_allocation.h"
#include "components/viz/common/resources/shared_bitmap.h"
#include "components/viz/service/display/display_resource_provider.h"
#include "components/viz/service/display/gl_renderer.h"
#include "components/viz/service/display/output_surface_client.h"
#include "components/viz/service/display/software_output_device.h"
#include "components/viz/service/display/software_renderer.h"
#include "components/viz/service/display_embedder/in_process_gpu_memory_buffer_manager.h"
#include "components/viz/service/display_embedder/skia_output_surface_impl.h"
#include "components/viz/service/display_embedder/viz_process_context_provider.h"
#include "components/viz/service/gl/gpu_service_impl.h"
#include "components/viz/test/paths.h"
#include "components/viz/test/test_shared_bitmap_manager.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/client/shared_memory_limits.h"
#include "gpu/command_buffer/service/service_utils.h"
#include "gpu/config/gpu_feature_type.h"
#include "gpu/config/gpu_info.h"
#include "gpu/ipc/gpu_in_process_thread_service.h"
#include "gpu/ipc/service/gpu_memory_buffer_factory.h"
#include "gpu/ipc/service/gpu_watchdog_thread.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "services/viz/privileged/interfaces/gl/gpu_host.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gl/init/gl_factory.h"

#if BUILDFLAG(ENABLE_VULKAN)
#include "gpu/vulkan/init/vulkan_factory.h"
#include "gpu/vulkan/vulkan_implementation.h"
#endif

namespace cc {
namespace {

// A wrapper around SkiaOutputSurfaceImpl that can be used to change settings
// for tests.
class PixelTestSkiaOutputSurfaceImpl : public viz::SkiaOutputSurfaceImpl {
 public:
  PixelTestSkiaOutputSurfaceImpl(
      viz::GpuServiceImpl* gpu_service,
      gpu::SurfaceHandle surface_handle,
      viz::SyntheticBeginFrameSource* synthetic_begin_frame_source,
      const viz::RendererSettings& renderer_settings,
      bool flipped_output_surface)
      : SkiaOutputSurfaceImpl(gpu_service,
                              surface_handle,
                              synthetic_begin_frame_source,
                              renderer_settings),
        flipped_output_surface_(flipped_output_surface) {}

  // |capabilities_| is set in InitializeForGL(), so wrap BindToClient() and set
  // |flipped_output_surface| once that is complete.
  void BindToClient(viz::OutputSurfaceClient* client) override {
    SkiaOutputSurfaceImpl::BindToClient(client);
    SetCapabilitiesForTesting(flipped_output_surface_);
  }

 private:
  const bool flipped_output_surface_;
};

}  // namespace

PixelTest::PixelTest()
    : device_viewport_size_(gfx::Size(200, 200)),
      disable_picture_quad_image_filtering_(false),
      output_surface_client_(std::make_unique<FakeOutputSurfaceClient>()) {
  // Keep texture sizes exactly matching the bounds of the RenderPass to avoid
  // floating point badness in texcoords.
  renderer_settings_.dont_round_texture_sizes_for_pixel_tests = true;
}

PixelTest::~PixelTest() = default;

bool PixelTest::RunPixelTest(viz::RenderPassList* pass_list,
                             const base::FilePath& ref_file,
                             const PixelComparator& comparator) {
  return RunPixelTestWithReadbackTarget(pass_list, pass_list->back().get(),
                                        ref_file, comparator);
}

bool PixelTest::RunPixelTestWithReadbackTarget(
    viz::RenderPassList* pass_list,
    viz::RenderPass* target,
    const base::FilePath& ref_file,
    const PixelComparator& comparator) {
  return RunPixelTestWithReadbackTargetAndArea(
      pass_list, target, ref_file, comparator, nullptr);
}

bool PixelTest::RunPixelTestWithReadbackTargetAndArea(
    viz::RenderPassList* pass_list,
    viz::RenderPass* target,
    const base::FilePath& ref_file,
    const PixelComparator& comparator,
    const gfx::Rect* copy_rect) {
  base::RunLoop run_loop;

  std::unique_ptr<viz::CopyOutputRequest> request =
      std::make_unique<viz::CopyOutputRequest>(
          viz::CopyOutputRequest::ResultFormat::RGBA_BITMAP,
          base::BindOnce(&PixelTest::ReadbackResult, base::Unretained(this),
                         run_loop.QuitClosure()));
  if (copy_rect)
    request->set_area(*copy_rect);
  target->copy_requests.push_back(std::move(request));

  if (software_renderer_) {
    software_renderer_->SetDisablePictureQuadImageFiltering(
        disable_picture_quad_image_filtering_);
  }

  renderer_->DecideRenderPassAllocationsForFrame(*pass_list);
  float device_scale_factor = 1.f;
  renderer_->DrawFrame(pass_list, device_scale_factor, device_viewport_size_);

  // Wait for the readback to complete.
  if (output_surface_->context_provider())
    output_surface_->context_provider()->ContextGL()->Finish();
  run_loop.Run();

  return PixelsMatchReference(ref_file, comparator);
}

bool PixelTest::RunPixelTest(viz::RenderPassList* pass_list,
                             std::vector<SkColor>* ref_pixels,
                             const PixelComparator& comparator) {
  base::RunLoop run_loop;
  viz::RenderPass* target = pass_list->back().get();

  std::unique_ptr<viz::CopyOutputRequest> request =
      std::make_unique<viz::CopyOutputRequest>(
          viz::CopyOutputRequest::ResultFormat::RGBA_BITMAP,
          base::BindOnce(&PixelTest::ReadbackResult, base::Unretained(this),
                         run_loop.QuitClosure()));
  target->copy_requests.push_back(std::move(request));

  if (software_renderer_) {
    software_renderer_->SetDisablePictureQuadImageFiltering(
        disable_picture_quad_image_filtering_);
  }

  renderer_->DecideRenderPassAllocationsForFrame(*pass_list);
  float device_scale_factor = 1.f;
  renderer_->DrawFrame(pass_list, device_scale_factor, device_viewport_size_);

  // Wait for the readback to complete.
  if (output_surface_->context_provider())
    output_surface_->context_provider()->ContextGL()->Finish();
  run_loop.Run();

  // Need to wrap |ref_pixels| in a SkBitmap.
  DCHECK_EQ(ref_pixels->size(), static_cast<size_t>(result_bitmap_->width() *
                                                    result_bitmap_->height()));
  SkBitmap ref_pixels_bitmap;
  ref_pixels_bitmap.installPixels(
      SkImageInfo::MakeN32Premul(result_bitmap_->width(),
                                 result_bitmap_->height()),
      ref_pixels->data(), result_bitmap_->width() * sizeof(SkColor));
  return comparator.Compare(*result_bitmap_, ref_pixels_bitmap);
}

void PixelTest::ReadbackResult(base::OnceClosure quit_run_loop,
                               std::unique_ptr<viz::CopyOutputResult> result) {
  ASSERT_FALSE(result->IsEmpty());
  EXPECT_EQ(result->format(), viz::CopyOutputResult::Format::RGBA_BITMAP);
  result_bitmap_ = std::make_unique<SkBitmap>(result->AsSkBitmap());
  EXPECT_TRUE(result_bitmap_->readyToDraw());
  std::move(quit_run_loop).Run();
}

bool PixelTest::PixelsMatchReference(const base::FilePath& ref_file,
                                     const PixelComparator& comparator) {
  base::FilePath test_data_dir;
  if (!base::PathService::Get(viz::Paths::DIR_TEST_DATA, &test_data_dir))
    return false;

  // If this is false, we didn't set up a readback on a render pass.
  if (!result_bitmap_)
    return false;

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(switches::kCCRebaselinePixeltests))
    return WritePNGFile(*result_bitmap_, test_data_dir.Append(ref_file), true);

  return MatchesPNGFile(
      *result_bitmap_, test_data_dir.Append(ref_file), comparator);
}

std::unique_ptr<base::SharedMemory> PixelTest::AllocateSharedBitmapMemory(
    const viz::SharedBitmapId& id,
    const gfx::Size& size) {
  std::unique_ptr<base::SharedMemory> shm =
      viz::bitmap_allocation::AllocateMappedBitmap(size, viz::RGBA_8888);
  this->shared_bitmap_manager_->ChildAllocatedSharedBitmap(
      viz::bitmap_allocation::DuplicateAndCloseMappedBitmap(shm.get(), size,
                                                            viz::RGBA_8888),
      id);
  return shm;
}

viz::ResourceId PixelTest::AllocateAndFillSoftwareResource(
    const gfx::Size& size,
    const SkBitmap& source) {
  viz::SharedBitmapId shared_bitmap_id = viz::SharedBitmap::GenerateId();
  std::unique_ptr<base::SharedMemory> shm =
      AllocateSharedBitmapMemory(shared_bitmap_id, size);

  SkImageInfo info = SkImageInfo::MakeN32Premul(size.width(), size.height());
  source.readPixels(info, shm->memory(), info.minRowBytes(), 0, 0);

  return child_resource_provider_->ImportResource(
      viz::TransferableResource::MakeSoftware(shared_bitmap_id, size,
                                              viz::RGBA_8888),
      viz::SingleReleaseCallback::Create(base::DoNothing()));
}

void PixelTest::SetUpGLWithoutRenderer(bool flipped_output_surface) {
  enable_pixel_output_ = std::make_unique<gl::DisableNullDrawGLBindings>();

  auto context_provider = base::MakeRefCounted<TestInProcessContextProvider>(
      /*enable_oop_rasterization=*/false, /*support_locking=*/false);
  gpu::ContextResult result = context_provider->BindToCurrentThread();
  DCHECK_EQ(result, gpu::ContextResult::kSuccess);
  output_surface_ = std::make_unique<PixelTestOutputSurface>(
      std::move(context_provider), flipped_output_surface);
  output_surface_->BindToClient(output_surface_client_.get());

  shared_bitmap_manager_ = std::make_unique<viz::TestSharedBitmapManager>();
  resource_provider_ = std::make_unique<viz::DisplayResourceProvider>(
      viz::DisplayResourceProvider::kGpu, output_surface_->context_provider(),
      shared_bitmap_manager_.get());

  child_context_provider_ = base::MakeRefCounted<TestInProcessContextProvider>(
      /*enable_oop_rasterization=*/false, /*support_locking=*/false);
  result = child_context_provider_->BindToCurrentThread();
  DCHECK_EQ(result, gpu::ContextResult::kSuccess);
  constexpr bool sync_token_verification = false;
  child_resource_provider_ =
      std::make_unique<viz::ClientResourceProvider>(sync_token_verification);
}

void PixelTest::SetUpGLRenderer(bool flipped_output_surface) {
  SetUpGLWithoutRenderer(flipped_output_surface);
  renderer_ = std::make_unique<viz::GLRenderer>(
      &renderer_settings_, output_surface_.get(), resource_provider_.get(),
      base::ThreadTaskRunnerHandle::Get());
  renderer_->Initialize();
  renderer_->SetVisible(true);
}

void PixelTest::SetUpGpuServiceOnGpuThread(base::WaitableEvent* event) {
  ASSERT_TRUE(gpu_thread_->task_runner()->BelongsToCurrentThread());
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  gpu::GpuPreferences gpu_preferences =
      gpu::gles2::ParseGpuPreferences(command_line);
  if (gpu_preferences.enable_vulkan) {
#if BUILDFLAG(ENABLE_VULKAN)
    vulkan_implementation_ = gpu::CreateVulkanImplementation();
    if (!vulkan_implementation_ ||
        !vulkan_implementation_->InitializeVulkanInstance(
            !gpu_preferences.disable_vulkan_surface)) {
      LOG(FATAL) << "Failed to create and initialize Vulkan implementation.";
    }
#else
    NOTREACHED();
#endif
  }
  gpu::GpuFeatureInfo gpu_feature_info;
  // To test SkiaRenderer with DDL, we need enable OOP-R.
  gpu_feature_info.status_values[gpu::GPU_FEATURE_TYPE_OOP_RASTERIZATION] =
      gpu::kGpuFeatureStatusEnabled;
  gpu_service_ = std::make_unique<viz::GpuServiceImpl>(
      gpu::GPUInfo(), nullptr /* watchdog_thread */, io_thread_->task_runner(),
      gpu_feature_info, gpu_preferences,
      gpu::GPUInfo() /* gpu_info_for_hardware_gpu */,
      gpu::GpuFeatureInfo() /* gpu_feature_info_for_hardware_gpu */,
#if BUILDFLAG(ENABLE_VULKAN)
      vulkan_implementation_.get(),
#else
      nullptr /* vulkan_implementation */,
#endif
      base::DoNothing() /* exit_callback */);

  // Uses a null gpu_host here, because we don't want to receive any message.
  std::unique_ptr<viz::mojom::GpuHost> gpu_host;
  viz::mojom::GpuHostPtr gpu_host_proxy;
  mojo::MakeStrongBinding(std::move(gpu_host),
                          mojo::MakeRequest(&gpu_host_proxy));
  gpu_service_->InitializeWithHost(
      std::move(gpu_host_proxy), gpu::GpuProcessActivityFlags(),
      gl::init::CreateOffscreenGLSurface(gfx::Size()),
      nullptr /* sync_point_manager */, nullptr /* shared_image_manager */,
      nullptr /* shutdown_event */);
  task_executor_ = std::make_unique<gpu::GpuInProcessThreadService>(
      gpu_thread_->task_runner(), gpu_service_->scheduler(),
      gpu_service_->sync_point_manager(), gpu_service_->mailbox_manager(),
      gpu_service_->share_group(),
      gpu_service_->gpu_channel_manager()
          ->default_offscreen_surface()
          ->GetFormat(),
      gpu_service_->gpu_feature_info(),
      gpu_service_->gpu_channel_manager()->gpu_preferences(),
      gpu_service_->shared_image_manager(),
      gpu_service_->gpu_channel_manager()->program_cache());
  event->Signal();
}

void PixelTest::SetUpSkiaRenderer(bool flipped_output_surface) {
  // Set up the GPU service.
  const char enable_features[] = "VizDisplayCompositor,UseSkiaRenderer";
  const char disable_features[] = "";
  scoped_feature_list_ = std::make_unique<base::test::ScopedFeatureList>();
  scoped_feature_list_->InitFromCommandLine(enable_features, disable_features);

  gpu_thread_ = std::make_unique<base::Thread>("GPUMainThread");
  ASSERT_TRUE(gpu_thread_->Start());
  io_thread_ = std::make_unique<base::Thread>("GPUIOThread");
  ASSERT_TRUE(io_thread_->Start());
  base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                            base::WaitableEvent::InitialState::NOT_SIGNALED);
  gpu_thread_->task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&PixelTest::SetUpGpuServiceOnGpuThread,
                                base::Unretained(this), &event));
  event.Wait();

  // Set up the skia renderer.
  output_surface_ = std::make_unique<PixelTestSkiaOutputSurfaceImpl>(
      gpu_service_.get(), gpu::kNullSurfaceHandle,
      nullptr /* synthetic_begin_frame_source */, renderer_settings_,
      flipped_output_surface);
  output_surface_->BindToClient(output_surface_client_.get());
  resource_provider_ = std::make_unique<viz::DisplayResourceProvider>(
      viz::DisplayResourceProvider::kGpu,
      nullptr /* compositor_context_provider */,
      nullptr /* shared_bitmap_manager */);
  renderer_ = std::make_unique<viz::SkiaRenderer>(
      &renderer_settings_, output_surface_.get(), resource_provider_.get(),
      static_cast<viz::SkiaOutputSurface*>(output_surface_.get()),
      viz::SkiaRenderer::DrawMode::DDL);
  renderer_->Initialize();
  renderer_->SetVisible(true);

  // Set up the client side context provider, etc
  gpu_memory_buffer_manager_ =
      std::make_unique<viz::InProcessGpuMemoryBufferManager>(
          gpu_service_->gpu_memory_buffer_factory(),
          gpu_service_->sync_point_manager());
  gpu::ImageFactory* image_factory = gpu_service_->gpu_image_factory();
  auto* gpu_channel_manager_delegate =
      gpu_service_->gpu_channel_manager()->delegate();
  viz::RendererSettings renderer_settings;
  renderer_settings.requires_alpha_channel = false;
#if defined(OS_ANDROID)
  // Pick a reasonable arbitrary size for tests - used to set memory limits.
  renderer_settings.initial_screen_size = gfx::Size(1920, 1080);
  renderer_settings.color_space = gfx::ColorSpace::CreateSRGB();
#endif
  child_context_provider_ =
      base::MakeRefCounted<viz::VizProcessContextProvider>(
          task_executor_.get(), gpu::kNullSurfaceHandle,
          gpu_memory_buffer_manager_.get(), image_factory,
          gpu_channel_manager_delegate, renderer_settings);
  child_context_provider_->BindToCurrentThread();
  constexpr bool sync_token_verification = false;
  child_resource_provider_ =
      std::make_unique<viz::ClientResourceProvider>(sync_token_verification);
}

void PixelTest::TearDownGpuServiceOnGpuThread(base::WaitableEvent* event) {
  task_executor_ = nullptr;
  gpu_service_ = nullptr;
  event->Signal();
}

void PixelTest::TearDown() {
  // Tear down the client side context provider, etc.
  child_resource_provider_->ShutdownAndReleaseAllResources();
  child_resource_provider_ = nullptr;
  child_context_provider_ = nullptr;
  gpu_memory_buffer_manager_ = nullptr;

  // Tear down the skia renderer.
  renderer_ = nullptr;
  resource_provider_ = nullptr;
  output_surface_ = nullptr;

  if (task_executor_) {
    // Tear down the GPU service.
    base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                              base::WaitableEvent::InitialState::NOT_SIGNALED);
    gpu_thread_->task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&PixelTest::TearDownGpuServiceOnGpuThread,
                                  base::Unretained(this), &event));
    event.Wait();
  }
  io_thread_ = nullptr;
  gpu_thread_ = nullptr;
  scoped_feature_list_ = nullptr;
}

void PixelTest::EnableExternalStencilTest() {
  static_cast<PixelTestOutputSurface*>(output_surface_.get())
      ->set_has_external_stencil_test(true);
}

void PixelTest::SetUpSoftwareRenderer() {
  output_surface_.reset(new PixelTestOutputSurface(
      std::make_unique<viz::SoftwareOutputDevice>()));
  output_surface_->BindToClient(output_surface_client_.get());
  shared_bitmap_manager_ = std::make_unique<viz::TestSharedBitmapManager>();
  resource_provider_ = std::make_unique<viz::DisplayResourceProvider>(
      viz::DisplayResourceProvider::kSoftware, nullptr,
      shared_bitmap_manager_.get());
  constexpr bool sync_token_verification = false;
  child_resource_provider_ =
      std::make_unique<viz::ClientResourceProvider>(sync_token_verification);

  auto renderer = std::make_unique<viz::SoftwareRenderer>(
      &renderer_settings_, output_surface_.get(), resource_provider_.get());
  software_renderer_ = renderer.get();
  renderer_ = std::move(renderer);
  renderer_->Initialize();
  renderer_->SetVisible(true);
}

}  // namespace cc

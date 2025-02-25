// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/vr/arcore_device/arcore_device.h"

#include "base/bind.h"
#include "base/numerics/math_constants.h"
#include "base/optional.h"
#include "base/task/post_task.h"
#include "base/trace_event/trace_event.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/android/vr/arcore_device/ar_image_transport.h"
#include "chrome/browser/android/vr/arcore_device/arcore_gl.h"
#include "chrome/browser/android/vr/arcore_device/arcore_gl_thread.h"
#include "chrome/browser/android/vr/arcore_device/arcore_impl.h"
#include "chrome/browser/android/vr/arcore_device/arcore_install_utils.h"
#include "chrome/browser/android/vr/arcore_device/arcore_java_utils.h"
#include "chrome/browser/android/vr/arcore_device/arcore_permission_helper.h"
#include "chrome/browser/android/vr/mailbox_to_surface_bridge.h"
#include "chrome/browser/permissions/permission_manager.h"
#include "chrome/browser/permissions/permission_result.h"
#include "chrome/browser/permissions/permission_update_infobar_delegate_android.h"
#include "device/vr/vr_display_impl.h"
#include "ui/display/display.h"

using base::android::JavaRef;

namespace {
constexpr float kDegreesPerRadian = 180.0f / base::kPiFloat;
}  // namespace

namespace device {

namespace {

mojom::VRDisplayInfoPtr CreateVRDisplayInfo(mojom::XRDeviceId device_id) {
  mojom::VRDisplayInfoPtr device = mojom::VRDisplayInfo::New();
  device->id = device_id;
  device->displayName = "ARCore VR Device";
  device->capabilities = mojom::VRDisplayCapabilities::New();
  device->capabilities->hasPosition = true;
  device->capabilities->hasExternalDisplay = false;
  device->capabilities->canPresent = false;
  device->capabilities->canProvideEnvironmentIntegration = true;
  device->leftEye = mojom::VREyeParameters::New();
  device->rightEye = nullptr;
  mojom::VREyeParametersPtr& left_eye = device->leftEye;
  left_eye->fieldOfView = mojom::VRFieldOfView::New();
  // TODO(lincolnfrog): get these values for real (see gvr device).
  uint width = 1080;
  uint height = 1795;
  double fov_x = 1437.387;
  double fov_y = 1438.074;
  // TODO(lincolnfrog): get real camera intrinsics.
  float horizontal_degrees = atan(width / (2.0 * fov_x)) * kDegreesPerRadian;
  float vertical_degrees = atan(height / (2.0 * fov_y)) * kDegreesPerRadian;
  left_eye->fieldOfView->leftDegrees = horizontal_degrees;
  left_eye->fieldOfView->rightDegrees = horizontal_degrees;
  left_eye->fieldOfView->upDegrees = vertical_degrees;
  left_eye->fieldOfView->downDegrees = vertical_degrees;
  left_eye->offset = {0.0f, 0.0f, 0.0f};
  left_eye->renderWidth = width;
  left_eye->renderHeight = height;
  return device;
}

}  // namespace

ArCoreDevice::ArCoreDevice(
    std::unique_ptr<ArCoreFactory> arcore_factory,
    std::unique_ptr<ArImageTransportFactory> ar_image_transport_factory,
    std::unique_ptr<vr::MailboxToSurfaceBridge> mailbox_to_surface_bridge,
    std::unique_ptr<vr::ArCoreInstallUtils> arcore_install_utils,
    std::unique_ptr<ArCorePermissionHelper> arcore_permission_helper)
    : VRDeviceBase(mojom::XRDeviceId::ARCORE_DEVICE_ID),
      main_thread_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      arcore_factory_(std::move(arcore_factory)),
      ar_image_transport_factory_(std::move(ar_image_transport_factory)),
      mailbox_bridge_(std::move(mailbox_to_surface_bridge)),
      arcore_install_utils_(std::move(arcore_install_utils)),
      arcore_permission_helper_(std::move(arcore_permission_helper)),
      weak_ptr_factory_(this) {
  SetVRDisplayInfo(CreateVRDisplayInfo(GetId()));

  // TODO(https://crbug.com/836524) clean up usage of mailbox bridge
  // and extract the methods in this class that interact with ARCore API
  // into a separate class that implements the ArCore interface.
  mailbox_bridge_->CreateUnboundContextProvider(
      base::BindOnce(&ArCoreDevice::OnMailboxBridgeReady, GetWeakPtr()));
}

ArCoreDevice::ArCoreDevice()
    : ArCoreDevice(std::make_unique<ArCoreImplFactory>(),
                   std::make_unique<ArImageTransportFactory>(),
                   std::make_unique<vr::MailboxToSurfaceBridge>(),
                   std::make_unique<vr::ArCoreJavaUtils>(
                       base::BindRepeating(
                           &ArCoreDevice::OnRequestInstallArModuleResult,
                           base::Unretained(
                               this)),  // unretained is fine since ArCoreDevice
                                        // owns the ArCoreJavaUtils instance
                       base::BindRepeating(
                           &ArCoreDevice::OnRequestInstallSupportedArCoreResult,
                           base::Unretained(this))),  // ditto
                   std::make_unique<ArCorePermissionHelper>()) {}

ArCoreDevice::~ArCoreDevice() {
  CallDeferredRequestSessionCallbacks(/*success=*/false);
  // The GL thread must be terminated since it uses our members. For example,
  // there might still be a posted Initialize() call in flight that uses
  // arcore_install_utils_ and arcore_factory_. Ensure that the thread is
  // stopped before other members get destructed. Don't call Stop() here,
  // destruction calls Stop() and doing so twice is illegal (null pointer
  // dereference).
  arcore_gl_thread_ = nullptr;
}

void ArCoreDevice::OnMailboxBridgeReady() {
  DCHECK(IsOnMainThread());
  DCHECK(!arcore_gl_thread_);
  // MailboxToSurfaceBridge's destructor's call to DestroyContext must
  // happen on the GL thread, so transferring it to that thread is appropriate.
  // TODO(https://crbug.com/836553): use same GL thread as GVR.
  arcore_gl_thread_ = std::make_unique<ArCoreGlThread>(
      std::move(ar_image_transport_factory_), std::move(mailbox_bridge_),
      CreateMainThreadCallback(base::BindOnce(
          &ArCoreDevice::OnArCoreGlThreadInitialized, GetWeakPtr())));
  arcore_gl_thread_->Start();
}

void ArCoreDevice::OnArCoreGlThreadInitialized() {
  DCHECK(IsOnMainThread());

  is_arcore_gl_thread_initialized_ = true;

  if (pending_request_ar_module_callback_) {
    std::move(pending_request_ar_module_callback_).Run();
  }
}

void ArCoreDevice::RequestSession(
    mojom::XRRuntimeSessionOptionsPtr options,
    mojom::XRRuntime::RequestSessionCallback callback) {
  DCHECK(IsOnMainThread());

  // If we are currently handling another request defer this request. All
  // deferred requests will be processed once handling is complete.
  deferred_request_session_callbacks_.push_back(std::move(callback));
  if (deferred_request_session_callbacks_.size() > 1) {
    return;
  }

  // TODO(https://crbug.com/849568): Instead of splitting the initialization
  // of this class between construction and RequestSession, perform all the
  // initialization at once on the first successful RequestSession call.
  if (!is_arcore_gl_thread_initialized_) {
    pending_request_ar_module_callback_ =
        base::BindOnce(&ArCoreDevice::RequestArModule, GetWeakPtr(),
                       options->render_process_id, options->render_frame_id,
                       options->has_user_activation);
    return;
  }

  RequestArModule(options->render_process_id, options->render_frame_id,
                  options->has_user_activation);
}

void ArCoreDevice::RequestArModule(int render_process_id,
                                   int render_frame_id,
                                   bool has_user_activation) {
  if (arcore_install_utils_->ShouldRequestInstallArModule()) {
    if (!arcore_install_utils_->CanRequestInstallArModule()) {
      OnRequestArModuleResult(render_process_id, render_frame_id,
                              has_user_activation, false);
      return;
    }

    on_request_ar_module_result_callback_ =
        base::BindOnce(&ArCoreDevice::OnRequestArModuleResult, GetWeakPtr(),
                       render_process_id, render_frame_id, has_user_activation);
    arcore_install_utils_->RequestInstallArModule(render_process_id,
                                                  render_frame_id);
    return;
  }

  OnRequestArModuleResult(render_process_id, render_frame_id,
                          has_user_activation, true);
}

void ArCoreDevice::OnRequestArModuleResult(int render_process_id,
                                           int render_frame_id,
                                           bool has_user_activation,
                                           bool success) {
  DVLOG(3) << __func__ << ": has_user_activation=" << has_user_activation
           << ", success=" << success;

  if (!success) {
    CallDeferredRequestSessionCallbacks(/*success=*/false);
    return;
  }

  RequestArCoreInstallOrUpdate(render_process_id, render_frame_id,
                               has_user_activation);
}

void ArCoreDevice::RequestArCoreInstallOrUpdate(int render_process_id,
                                                int render_frame_id,
                                                bool has_user_activation) {
  DCHECK(IsOnMainThread());
  DCHECK(is_arcore_gl_thread_initialized_);
  DCHECK(!on_request_arcore_install_or_update_result_callback_);

  if (arcore_install_utils_->ShouldRequestInstallSupportedArCore()) {
    // ARCore is not installed or requires an update. Store the callback to be
    // processed later once installation/update is complete or got cancelled.
    on_request_arcore_install_or_update_result_callback_ = base::BindOnce(
        &ArCoreDevice::OnRequestArCoreInstallOrUpdateResult, GetWeakPtr(),
        render_process_id, render_frame_id, has_user_activation);

    arcore_install_utils_->RequestInstallSupportedArCore(render_process_id,
                                                         render_frame_id);
    return;
  }

  OnRequestArCoreInstallOrUpdateResult(render_process_id, render_frame_id,
                                       has_user_activation, true);
}

void ArCoreDevice::OnRequestArCoreInstallOrUpdateResult(
    int render_process_id,
    int render_frame_id,
    bool has_user_activation,
    bool success) {
  DCHECK(IsOnMainThread());
  DCHECK(is_arcore_gl_thread_initialized_);

  if (!success) {
    CallDeferredRequestSessionCallbacks(/*success=*/false);
    return;
  }

  // TODO(https://crbug.com/845792): Consider calling a method to ask for the
  // appropriate permissions.
  // ARCore sessions require camera permission.
  arcore_permission_helper_->RequestCameraPermission(
      render_process_id, render_frame_id, has_user_activation,
      base::BindOnce(&ArCoreDevice::OnRequestCameraPermissionComplete,
                     GetWeakPtr()));
}

void ArCoreDevice::OnRequestCameraPermissionComplete(bool success) {
  DCHECK(IsOnMainThread());
  DCHECK(is_arcore_gl_thread_initialized_);

  if (!success) {
    CallDeferredRequestSessionCallbacks(/*success=*/false);
    return;
  }

  // By this point ARCore has already been set up, so continue handling request.
  RequestArCoreGlInitialization();
}

void ArCoreDevice::OnRequestInstallArModuleResult(bool success) {
  DCHECK(IsOnMainThread());

  if (on_request_ar_module_result_callback_) {
    std::move(on_request_ar_module_result_callback_).Run(success);
  }
}

void ArCoreDevice::OnRequestInstallSupportedArCoreResult(bool success) {
  DCHECK(IsOnMainThread());
  DCHECK(is_arcore_gl_thread_initialized_);
  DCHECK(on_request_arcore_install_or_update_result_callback_);

  std::move(on_request_arcore_install_or_update_result_callback_).Run(success);
}

void ArCoreDevice::CallDeferredRequestSessionCallbacks(bool success) {
  DCHECK(IsOnMainThread());
  DCHECK(!success || is_arcore_gl_thread_initialized_);

  for (auto& deferred_callback : deferred_request_session_callbacks_) {
    // We don't expect this call to alter deferred_request_session_callbacks_.
    // The call may request another session, which should be handled right here
    // in this loop as well.

    if (!success) {
      std::move(deferred_callback).Run(nullptr, nullptr);
      continue;
    }

    auto callback = base::BindOnce(&ArCoreDevice::OnCreateSessionCallback,
                                   GetWeakPtr(), std::move(deferred_callback));

    PostTaskToGlThread(base::BindOnce(
        &ArCoreGl::CreateSession,
        arcore_gl_thread_->GetArCoreGl()->GetWeakPtr(), display_info_->Clone(),
        CreateMainThreadCallback(std::move(callback))));
  }
  deferred_request_session_callbacks_.clear();
}

void ArCoreDevice::OnCreateSessionCallback(
    mojom::XRRuntime::RequestSessionCallback deferred_callback,
    mojom::XRFrameDataProviderPtrInfo frame_data_provider_info,
    mojom::VRDisplayInfoPtr display_info,
    mojom::XRSessionControllerPtrInfo session_controller_info) {
  DCHECK(IsOnMainThread());

  mojom::XRSessionPtr session = mojom::XRSession::New();
  session->data_provider = std::move(frame_data_provider_info);
  session->display_info = std::move(display_info);

  mojom::XRSessionControllerPtr controller(std::move(session_controller_info));

  std::move(deferred_callback).Run(std::move(session), std::move(controller));
}

void ArCoreDevice::PostTaskToGlThread(base::OnceClosure task) {
  DCHECK(IsOnMainThread());
  arcore_gl_thread_->GetArCoreGl()->GetGlThreadTaskRunner()->PostTask(
      FROM_HERE, std::move(task));
}

bool ArCoreDevice::IsOnMainThread() {
  return main_thread_task_runner_->BelongsToCurrentThread();
}

void ArCoreDevice::RequestArCoreGlInitialization() {
  DCHECK(IsOnMainThread());
  DCHECK(is_arcore_gl_thread_initialized_);

  if (!arcore_install_utils_->EnsureLoaded()) {
    DLOG(ERROR) << "ARCore was not loaded properly.";
    OnArCoreGlInitializationComplete(false);
    return;
  }

  if (!is_arcore_gl_initialized_) {
    // We will only try to initialize ArCoreGl once, at the end of the
    // permission sequence, and will resolve pending requests that have queued
    // up once that initialization completes. We set is_arcore_gl_initialized_
    // in the callback to block operations that require it to be ready.
    PostTaskToGlThread(base::BindOnce(
        &ArCoreGl::Initialize, arcore_gl_thread_->GetArCoreGl()->GetWeakPtr(),
        arcore_install_utils_.get(), arcore_factory_.get(),
        CreateMainThreadCallback(base::BindOnce(
            &ArCoreDevice::OnArCoreGlInitializationComplete, GetWeakPtr()))));
    return;
  }

  OnArCoreGlInitializationComplete(true);
}

void ArCoreDevice::OnArCoreGlInitializationComplete(bool success) {
  DCHECK(IsOnMainThread());
  DCHECK(is_arcore_gl_thread_initialized_);

  if (!success) {
    CallDeferredRequestSessionCallbacks(/*success=*/false);
    return;
  }

  is_arcore_gl_initialized_ = true;

  CallDeferredRequestSessionCallbacks(/*success=*/true);
}

}  // namespace device

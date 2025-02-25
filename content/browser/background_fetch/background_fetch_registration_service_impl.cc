// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/background_fetch/background_fetch_registration_service_impl.h"

#include <memory>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/guid.h"
#include "base/memory/ptr_util.h"
#include "base/optional.h"
#include "base/task/post_task.h"
#include "content/browser/background_fetch/background_fetch_context.h"
#include "content/browser/background_fetch/background_fetch_metrics.h"
#include "content/browser/background_fetch/background_fetch_registration_id.h"
#include "content/browser/background_fetch/background_fetch_registration_notifier.h"
#include "content/browser/background_fetch/background_fetch_request_match_params.h"
#include "content/browser/bad_message.h"
#include "content/browser/storage_partition_impl.h"
#include "content/common/service_worker/service_worker_types.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "mojo/public/cpp/bindings/interface_request.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom.h"

namespace content {

namespace {

// Maximum length of a developer-provided title for a Background Fetch.
constexpr size_t kMaxTitleLength = 1024 * 1024;

}  // namespace

// static
blink::mojom::BackgroundFetchRegistrationServicePtrInfo
BackgroundFetchRegistrationServiceImpl::CreateInterfaceInfo(
    BackgroundFetchRegistrationId registration_id,
    scoped_refptr<BackgroundFetchContext> background_fetch_context) {
  DCHECK(background_fetch_context);

  blink::mojom::BackgroundFetchRegistrationServicePtr interface;

  mojo::MakeStrongBinding(
      base::WrapUnique(new BackgroundFetchRegistrationServiceImpl(
          std::move(registration_id), std::move(background_fetch_context))),
      mojo::MakeRequest(&interface));

  return interface.PassInterface();
}

BackgroundFetchRegistrationServiceImpl::BackgroundFetchRegistrationServiceImpl(
    BackgroundFetchRegistrationId registration_id,
    scoped_refptr<BackgroundFetchContext> background_fetch_context)
    : registration_id_(std::move(registration_id)),
      background_fetch_context_(std::move(background_fetch_context)),
      binding_(this) {
  DCHECK(background_fetch_context_);
  DCHECK(!registration_id_.is_null());
}

BackgroundFetchRegistrationServiceImpl::
    ~BackgroundFetchRegistrationServiceImpl() = default;

void BackgroundFetchRegistrationServiceImpl::MatchRequests(
    blink::mojom::FetchAPIRequestPtr request_to_match,
    blink::mojom::CacheQueryOptionsPtr cache_query_options,
    bool match_all,
    MatchRequestsCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  // Create BackgroundFetchMatchRequestMatchParams.
  auto match_params = std::make_unique<BackgroundFetchRequestMatchParams>(
      std::move(request_to_match), std::move(cache_query_options), match_all);

  background_fetch_context_->MatchRequests(
      registration_id_, std::move(match_params), std::move(callback));
}

void BackgroundFetchRegistrationServiceImpl::UpdateUI(
    const base::Optional<std::string>& title,
    const SkBitmap& icon,
    UpdateUICallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (title && !ValidateTitle(*title)) {
    std::move(callback).Run(
        blink::mojom::BackgroundFetchError::INVALID_ARGUMENT);
    return;
  }

  // Wrap the icon in an optional for clarity.
  auto optional_icon =
      icon.isNull() ? base::nullopt : base::Optional<SkBitmap>(icon);

  background_fetch_context_->UpdateUI(registration_id_, title, optional_icon,
                                      std::move(callback));
}

void BackgroundFetchRegistrationServiceImpl::Abort(AbortCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  background_fetch_context_->Abort(registration_id_, std::move(callback));
}

void BackgroundFetchRegistrationServiceImpl::AddRegistrationObserver(
    blink::mojom::BackgroundFetchRegistrationObserverPtr observer) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  background_fetch_context_->AddRegistrationObserver(
      registration_id_.unique_id(), std::move(observer));
}

void BackgroundFetchRegistrationServiceImpl::Bind(
    blink::mojom::BackgroundFetchRegistrationServicePtr* interface_ptr) {
  binding_.Bind(mojo::MakeRequest(interface_ptr));
}

bool BackgroundFetchRegistrationServiceImpl::ValidateTitle(
    const std::string& title) {
  if (title.empty() || title.size() > kMaxTitleLength) {
    mojo::ReportBadMessage("Invalid title");
    return false;
  }

  return true;
}

}  // namespace content

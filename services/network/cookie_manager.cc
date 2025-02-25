// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/cookie_manager.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/process/process.h"
#include "build/build_config.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_monster.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_store.h"
#include "net/cookies/cookie_util.h"
#include "services/network/cookie_managers_shared.h"
#include "services/network/session_cleanup_cookie_store.h"
#include "url/gurl.h"

using CookieDeletionInfo = net::CookieDeletionInfo;
using CookieDeleteSessionControl = net::CookieDeletionInfo::SessionControl;

namespace network {

namespace {

bool g_crash_on_get_cookie_list = false;

}  // namespace

CookieManager::ListenerRegistration::ListenerRegistration() {}

CookieManager::ListenerRegistration::~ListenerRegistration() {}

void CookieManager::ListenerRegistration::DispatchCookieStoreChange(
    const net::CanonicalCookie& cookie,
    net::CookieChangeCause cause) {
  listener->OnCookieChange(cookie, ToCookieChangeCause(cause));
}

CookieManager::CookieManager(
    net::CookieStore* cookie_store,
    scoped_refptr<SessionCleanupCookieStore> session_cleanup_cookie_store,
    mojom::CookieManagerParamsPtr params)
    : cookie_store_(cookie_store),
      session_cleanup_cookie_store_(std::move(session_cleanup_cookie_store)) {
  if (params) {
    cookie_settings_.set_block_third_party_cookies(
        params->block_third_party_cookies);
    cookie_settings_.set_content_settings(params->settings);
    cookie_settings_.set_secure_origin_cookies_allowed_schemes(
        params->secure_origin_cookies_allowed_schemes);
    cookie_settings_.set_matching_scheme_cookies_allowed_schemes(
        params->matching_scheme_cookies_allowed_schemes);
    cookie_settings_.set_third_party_cookies_allowed_schemes(
        params->third_party_cookies_allowed_schemes);
    // Don't wait for callback, the work happens synchronously.
    AllowFileSchemeCookies(params->allow_file_scheme_cookies,
                           base::DoNothing());
  }
}

CookieManager::~CookieManager() {
  if (session_cleanup_cookie_store_) {
    session_cleanup_cookie_store_->DeleteSessionCookies(
        cookie_settings_.CreateDeleteCookieOnExitPredicate());
  }
}

void CookieManager::AddRequest(mojom::CookieManagerRequest request) {
  bindings_.AddBinding(this, std::move(request));
}

void CookieManager::GetAllCookies(GetAllCookiesCallback callback) {
  cookie_store_->GetAllCookiesAsync(
      net::cookie_util::IgnoreCookieStatusList(std::move(callback)));
}

void CookieManager::GetCookieList(const GURL& url,
                                  const net::CookieOptions& cookie_options,
                                  GetCookieListCallback callback) {
#if !defined(OS_IOS)
  if (g_crash_on_get_cookie_list)
    base::Process::TerminateCurrentProcessImmediately(1);
#endif

  cookie_store_->GetCookieListWithOptionsAsync(url, cookie_options,
                                               std::move(callback));
}

void CookieManager::SetCanonicalCookie(const net::CanonicalCookie& cookie,
                                       const std::string& source_scheme,
                                       const net::CookieOptions& cookie_options,
                                       SetCanonicalCookieCallback callback) {
  cookie_store_->SetCanonicalCookieAsync(
      std::make_unique<net::CanonicalCookie>(cookie), source_scheme,
      cookie_options, std::move(callback));
}

void CookieManager::DeleteCanonicalCookie(
    const net::CanonicalCookie& cookie,
    DeleteCanonicalCookieCallback callback) {
  cookie_store_->DeleteCanonicalCookieAsync(
      cookie,
      base::BindOnce(
          [](DeleteCanonicalCookieCallback callback, uint32_t num_deleted) {
            std::move(callback).Run(num_deleted > 0);
          },
          std::move(callback)));
}

void CookieManager::SetContentSettings(
    const ContentSettingsForOneType& settings) {
  cookie_settings_.set_content_settings(settings);
}

void CookieManager::DeleteCookies(mojom::CookieDeletionFilterPtr filter,
                                  DeleteCookiesCallback callback) {
  cookie_store_->DeleteAllMatchingInfoAsync(
      DeletionFilterToInfo(std::move(filter)), std::move(callback));
}

void CookieManager::AddCookieChangeListener(
    const GURL& url,
    const std::string& name,
    mojom::CookieChangeListenerPtr listener) {
  auto listener_registration = std::make_unique<ListenerRegistration>();
  listener_registration->listener = std::move(listener);

  listener_registration->subscription =
      cookie_store_->GetChangeDispatcher().AddCallbackForCookie(
          url, name,
          base::BindRepeating(
              &CookieManager::ListenerRegistration::DispatchCookieStoreChange,
              // base::Unretained is safe as destruction of the
              // ListenerRegistration will also destroy the
              // CookieChangedSubscription, unregistering the callback.
              base::Unretained(listener_registration.get())));

  listener_registration->listener.set_connection_error_handler(
      base::BindOnce(&CookieManager::RemoveChangeListener,
                     // base::Unretained is safe as destruction of the
                     // CookieManager will also destroy the
                     // notifications_registered list (which this object will be
                     // inserted into, below), which will destroy the
                     // listener, rendering this callback moot.
                     base::Unretained(this),
                     // base::Unretained is safe as destruction of the
                     // ListenerRegistration will also destroy the
                     // CookieChangedSubscription, unregistering the callback.
                     base::Unretained(listener_registration.get())));

  listener_registrations_.push_back(std::move(listener_registration));
}

void CookieManager::AddGlobalChangeListener(
    mojom::CookieChangeListenerPtr listener) {
  auto listener_registration = std::make_unique<ListenerRegistration>();
  listener_registration->listener = std::move(listener);

  listener_registration->subscription =
      cookie_store_->GetChangeDispatcher().AddCallbackForAllChanges(
          base::BindRepeating(
              &CookieManager::ListenerRegistration::DispatchCookieStoreChange,
              // base::Unretained is safe as destruction of the
              // ListenerRegistration will also destroy the
              // CookieChangedSubscription, unregistering the callback.
              base::Unretained(listener_registration.get())));

  listener_registration->listener.set_connection_error_handler(
      base::BindOnce(&CookieManager::RemoveChangeListener,
                     // base::Unretained is safe as destruction of the
                     // CookieManager will also destroy the
                     // notifications_registered list (which this object will be
                     // inserted into, below), which will destroy the
                     // listener, rendering this callback moot.
                     base::Unretained(this),
                     // base::Unretained is safe as destruction of the
                     // ListenerRegistration will also destroy the
                     // CookieChangedSubscription, unregistering the callback.
                     base::Unretained(listener_registration.get())));

  listener_registrations_.push_back(std::move(listener_registration));
}

void CookieManager::RemoveChangeListener(ListenerRegistration* registration) {
  for (auto it = listener_registrations_.begin();
       it != listener_registrations_.end(); ++it) {
    if (it->get() == registration) {
      // It isn't expected this will be a common enough operation for
      // the performance of std::vector::erase() to matter.
      listener_registrations_.erase(it);
      return;
    }
  }
  // A broken connection error should never be raised for an unknown pipe.
  NOTREACHED();
}

void CookieManager::CloneInterface(mojom::CookieManagerRequest new_interface) {
  AddRequest(std::move(new_interface));
}

void CookieManager::FlushCookieStore(FlushCookieStoreCallback callback) {
  // Flushes the backing store (if any) to disk.
  cookie_store_->FlushStore(std::move(callback));
}

void CookieManager::AllowFileSchemeCookies(
    bool allow,
    AllowFileSchemeCookiesCallback callback) {
  std::vector<std::string> cookieable_schemes(
      net::CookieMonster::kDefaultCookieableSchemes,
      net::CookieMonster::kDefaultCookieableSchemes +
          net::CookieMonster::kDefaultCookieableSchemesCount);
  if (allow) {
    cookieable_schemes.push_back(url::kFileScheme);
  }
  cookie_store_->SetCookieableSchemes(cookieable_schemes, std::move(callback));
}

void CookieManager::SetForceKeepSessionState() {
  cookie_store_->SetForceKeepSessionState();
}

void CookieManager::BlockThirdPartyCookies(bool block) {
  cookie_settings_.set_block_third_party_cookies(block);
}

void CookieManager::CrashOnGetCookieList() {
  g_crash_on_get_cookie_list = true;
}

CookieDeletionInfo DeletionFilterToInfo(mojom::CookieDeletionFilterPtr filter) {
  CookieDeletionInfo delete_info;

  if (filter->created_after_time.has_value() &&
      !filter->created_after_time.value().is_null()) {
    delete_info.creation_range.SetStart(filter->created_after_time.value());
  }
  if (filter->created_before_time.has_value() &&
      !filter->created_before_time.value().is_null()) {
    delete_info.creation_range.SetEnd(filter->created_before_time.value());
  }
  delete_info.name = std::move(filter->cookie_name);
  delete_info.url = std::move(filter->url);
  delete_info.host = std::move(filter->host_name);

  switch (filter->session_control) {
    case mojom::CookieDeletionSessionControl::IGNORE_CONTROL:
      delete_info.session_control = CookieDeleteSessionControl::IGNORE_CONTROL;
      break;
    case mojom::CookieDeletionSessionControl::SESSION_COOKIES:
      delete_info.session_control = CookieDeleteSessionControl::SESSION_COOKIES;
      break;
    case mojom::CookieDeletionSessionControl::PERSISTENT_COOKIES:
      delete_info.session_control =
          CookieDeleteSessionControl::PERSISTENT_COOKIES;
      break;
  }

  if (filter->including_domains.has_value()) {
    delete_info.domains_and_ips_to_delete.insert(
        filter->including_domains.value().begin(),
        filter->including_domains.value().end());
  }
  if (filter->excluding_domains.has_value()) {
    delete_info.domains_and_ips_to_ignore.insert(
        filter->excluding_domains.value().begin(),
        filter->excluding_domains.value().end());
  }

  return delete_info;
}

}  // namespace network

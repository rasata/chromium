// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WEB_APPLICATIONS_WEB_APP_PROVIDER_H_
#define CHROME_BROWSER_WEB_APPLICATIONS_WEB_APP_PROVIDER_H_

#include <memory>
#include <vector>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/web_applications/components/app_registrar.h"
#include "chrome/browser/web_applications/components/pending_app_manager.h"
#include "chrome/browser/web_applications/components/web_app_helpers.h"
#include "chrome/browser/web_applications/components/web_app_provider_base.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"

class Profile;

namespace content {
class WebContents;
}

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace web_app {

// Forward declarations of generalized interfaces.
class PendingAppManager;
class InstallManager;
class InstallFinalizer;
class WebAppAudioFocusIdMap;
class WebAppTabHelperBase;
class SystemWebAppManager;
class AppRegistrar;
class WebAppUiDelegate;

// Forward declarations for new extension-independent subsystems.
class WebAppDatabase;
class WebAppDatabaseFactory;
class WebAppIconManager;

// Forward declarations for legacy extension-based subsystems.
class WebAppPolicyManager;

// Connects Web App features, such as the installation of default and
// policy-managed web apps, with Profiles (as WebAppProvider is a
// Profile-linked KeyedService) and their associated PrefService.
class WebAppProvider : public WebAppProviderBase,
                       public content::NotificationObserver {
 public:
  static WebAppProvider* Get(Profile* profile);
  static WebAppProvider* GetForWebContents(content::WebContents* web_contents);

  explicit WebAppProvider(Profile* profile);
  ~WebAppProvider() override;

  // Create subsystems but do not start them (yet).
  void Init();
  // Start registry. All subsystems depend on it.
  void StartRegistry();

  // WebAppProviderBase:
  AppRegistrar& registrar() override;
  InstallManager& install_manager() override;
  PendingAppManager& pending_app_manager() override;
  WebAppPolicyManager* policy_manager() override;
  WebAppUiDelegate& ui_delegate() override;

  const SystemWebAppManager& system_web_app_manager() {
    return *system_web_app_manager_;
  }

  void set_ui_delegate(WebAppUiDelegate* ui_delegate) {
    ui_delegate_ = ui_delegate;
  }

  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);
  static WebAppTabHelperBase* CreateTabHelper(
      content::WebContents* web_contents);

  // content::NotificationObserver
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

  // Fires when app registry becomes ready.
  // Consider to use base::ObserverList or base::OneShotEvent if many
  // subscribers needed.
  void SetRegistryReadyCallback(base::OnceClosure callback);

  // Count a number of all apps which are installed by user (non-default).
  // Requires app registry to be in a ready state.
  int CountUserInstalledApps() const;

 protected:
  // Create extension-independent subsystems.
  void CreateWebAppsSubsystems(Profile* profile);
  // ... or create legacy extension-based subsystems.
  void CreateBookmarkAppsSubsystems(Profile* profile);

  void OnRegistryReady();

  void Reset();

  void OnScanForExternalWebApps(std::vector<InstallOptions>);

  // New extension-independent subsystems:
  std::unique_ptr<WebAppAudioFocusIdMap> audio_focus_id_map_;
  std::unique_ptr<WebAppDatabaseFactory> database_factory_;
  std::unique_ptr<WebAppDatabase> database_;
  std::unique_ptr<WebAppIconManager> icon_manager_;
  WebAppUiDelegate* ui_delegate_;

  // New generalized subsystems:
  std::unique_ptr<AppRegistrar> registrar_;
  std::unique_ptr<InstallFinalizer> install_finalizer_;
  std::unique_ptr<InstallManager> install_manager_;
  std::unique_ptr<PendingAppManager> pending_app_manager_;
  std::unique_ptr<SystemWebAppManager> system_web_app_manager_;

  // Legacy extension-based subsystems:
  std::unique_ptr<WebAppPolicyManager> web_app_policy_manager_;

  content::NotificationRegistrar notification_registrar_;

  base::OnceClosure registry_ready_callback_;
  bool registry_is_ready_ = false;

  Profile* profile_;

  base::WeakPtrFactory<WebAppProvider> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(WebAppProvider);
};

}  // namespace web_app

#endif  // CHROME_BROWSER_WEB_APPLICATIONS_WEB_APP_PROVIDER_H_

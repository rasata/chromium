// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_TEST_NAVIGATION_SIMULATOR_IMPL_H_
#define CONTENT_TEST_NAVIGATION_SIMULATOR_IMPL_H_

#include <memory>
#include <string>

#include "base/callback.h"
#include "base/optional.h"
#include "content/browser/frame_host/navigation_handle_impl.h"
#include "content/common/content_security_policy/csp_disposition_enum.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/referrer.h"
#include "content/public/test/navigation_simulator.h"
#include "mojo/public/cpp/bindings/associated_interface_request.h"
#include "net/base/host_port_pair.h"
#include "net/base/ip_endpoint.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/blink/public/mojom/frame/document_interface_broker.mojom.h"
#include "url/gurl.h"

struct FrameHostMsg_DidCommitProvisionalLoad_Params;

namespace content {

class FrameTreeNode;
class NavigationHandleImpl;
class NavigationRequest;
class TestRenderFrameHost;
class WebContentsImpl;

namespace mojom {
class NavigationClient;
}

class NavigationSimulatorImpl : public NavigationSimulator,
                                public WebContentsObserver {
 public:
  ~NavigationSimulatorImpl() override;

  static std::unique_ptr<NavigationSimulatorImpl> CreateBrowserInitiated(
      const GURL& original_url,
      WebContents* contents);

  static std::unique_ptr<NavigationSimulatorImpl> CreateHistoryNavigation(
      int offset,
      WebContents* web_contents);

  static std::unique_ptr<NavigationSimulatorImpl> CreateRendererInitiated(
      const GURL& original_url,
      RenderFrameHost* render_frame_host);

  static std::unique_ptr<NavigationSimulatorImpl> CreateFromPending(
      WebContents* contents);

  // NavigationSimulator implementation.
  void Start() override;
  void Redirect(const GURL& new_url) override;
  void ReadyToCommit() override;
  void Commit() override;
  void AbortCommit() override;
  void FailWithResponseHeaders(
      int error_code,
      scoped_refptr<net::HttpResponseHeaders> response_headers) override;
  void Fail(int error_code) override;
  void CommitErrorPage() override;
  void CommitSameDocument() override;
  RenderFrameHost* GetFinalRenderFrameHost() override;
  void Wait() override;
  bool IsDeferred() override;

  void SetTransition(ui::PageTransition transition) override;
  void SetHasUserGesture(bool has_user_gesture) override;
  void SetReloadType(ReloadType reload_type) override;
  void SetMethod(const std::string& method) override;
  void SetIsFormSubmission(bool is_form_submission) override;
  void SetReferrer(const Referrer& referrer) override;
  void SetSocketAddress(const net::IPEndPoint& remote_endpoint) override;
  void SetIsSignedExchangeInnerResponse(
      bool is_signed_exchange_inner_response) override;
  void SetInterfaceProviderRequest(
      service_manager::mojom::InterfaceProviderRequest request) override;
  void SetContentsMimeType(const std::string& contents_mime_type) override;
  void SetAutoAdvance(bool auto_advance) override;

  NavigationThrottle::ThrottleCheckResult GetLastThrottleCheckResult() override;
  NavigationHandleImpl* GetNavigationHandle() const override;
  content::GlobalRequestID GetGlobalRequestID() const override;

  // Additional utilites usable only inside content/.

  // This will do the very beginning of a navigation but stop before the
  // beforeunload event response. Will leave the Simulator in a
  // WAITING_BEFORE_UNLOAD state. We do not wait for beforeunload event when
  // starting renderer-side, use solely for browser initiated navigations.
  void BrowserInitiatedStartAndWaitBeforeUnload();

  // Set LoadURLParams and make browser initiated navigations use
  // LoadURLWithParams instead of LoadURL.
  void SetLoadURLParams(NavigationController::LoadURLParams* load_url_params);
  void set_should_check_main_world_csp(CSPDisposition disposition) {
    should_check_main_world_csp_ = disposition;
  }

  // Set DidCommit*Params history_list_was_cleared flag to |history_cleared|.
  void set_history_list_was_cleared(bool history_cleared);

  // Manually force the value of did_create_new_entry flag in DidCommit*Params
  // to |did_create_new_entry|.
  void set_did_create_new_entry(bool did_create_new_entry);

  // Manually force the value of should_replace_current_entry flag in
  // DidCommit*Params to |should_replace_current_entry|.
  void set_should_replace_current_entry(bool should_replace_current_entry) {
    should_replace_current_entry_ = should_replace_current_entry;
  }

  // Manually force the value of intended_as_new_entry flag in DidCommit*Params
  // to |intended_as_new_entry|.
  void set_intended_as_new_entry(bool intended_as_new_entry) {
    intended_as_new_entry_ = intended_as_new_entry;
  }

  void set_http_connection_info(net::HttpResponseInfo::ConnectionInfo info) {
    http_connection_info_ = info;
  }

  void set_ssl_info(net::SSLInfo ssl_info) { ssl_info_ = ssl_info; }

  // Whether to drop the swap out ack of the previous RenderFrameHost during
  // cross-process navigations. By default this is false, set to true if you
  // want the old RenderFrameHost to be left in a pending swap out state.
  void set_drop_swap_out_ack(bool drop_swap_out_ack) {
    drop_swap_out_ack_ = drop_swap_out_ack;
  }

  // Whether to drop the BeforeUnloadACK of the current RenderFrameHost at the
  // beginning of a browser-initiated navigation. By default this is false, set
  // to true if you want to simulate the BeforeUnloadACK manually.
  void set_block_on_before_unload_ack(bool block_on_before_unload_ack) {
    block_on_before_unload_ack_ = block_on_before_unload_ack;
  }

 private:
  NavigationSimulatorImpl(const GURL& original_url,
                          bool browser_initiated,
                          WebContentsImpl* web_contents,
                          TestRenderFrameHost* render_frame_host);

  // Adds a test navigation throttle to |handle| which sanity checks various
  // callbacks have been properly called.
  void RegisterTestThrottle(NavigationHandle* handle);

  // Initializes a NavigationSimulator from an existing NavigationRequest. This
  // should only be needed if a navigation was started without a valid
  // NavigationSimulator.
  void InitializeFromStartedRequest(NavigationRequest* request);

  // WebContentsObserver:
  void DidStartNavigation(NavigationHandle* navigation_handle) override;
  void DidRedirectNavigation(NavigationHandle* navigation_handle) override;
  void ReadyToCommitNavigation(NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(NavigationHandle* navigation_handle) override;

  void StartComplete();
  void RedirectComplete(int previous_num_will_redirect_request_called,
                        int previous_did_redirect_navigation_called);
  void ReadyToCommitComplete(bool ran_throttles);
  void FailComplete(int error_code);

  void OnWillStartRequest();
  void OnWillRedirectRequest();
  void OnWillFailRequest();
  void OnWillProcessResponse();

  // Simulates a browser-initiated navigation starting. Returns false if the
  // navigation failed synchronously.
  bool SimulateBrowserInitiatedStart();

  // Simulates a renderer-initiated navigation starting. Returns false if the
  // navigation failed synchronously.
  bool SimulateRendererInitiatedStart();

  // This method will block waiting for throttle checks to complete if
  // |auto_advance_|. Otherwise will just set up state for checking the result
  // when the throttles end up finishing.
  void MaybeWaitForThrottleChecksComplete(base::OnceClosure complete_closure);

  // Sets |last_throttle_check_result_| and calls both the
  // |wait_closure_| and the |throttle_checks_complete_closure_|, if they are
  // set.
  void OnThrottleChecksComplete(NavigationThrottle::ThrottleCheckResult result);

  // Helper method to set the OnThrottleChecksComplete callback on the
  // NavigationHandle.
  void PrepareCompleteCallbackOnHandle(NavigationHandleImpl* handle);

  // Check if the navigation corresponds to a same-document navigation.
  // Only use on renderer-initiated navigations.
  bool CheckIfSameDocument();

  // Infers from internal parameters whether the navigation created a new
  // entry.
  bool DidCreateNewEntry();

  // Set the navigation to be done towards the specified navigation controller
  // offset. Typically -1 for back navigations or 1 for forward navigations.
  void SetSessionHistoryOffset(int offset);

  // Build DidCommitProvisionalLoadParams to commit the ongoing navigation,
  // based on internal NavigationSimulator state and given parameters.
  std::unique_ptr<FrameHostMsg_DidCommitProvisionalLoad_Params>
  BuildDidCommitProvisionalLoadParams(bool same_document,
                                      bool failed_navigation);

  enum State {
    INITIALIZATION,
    WAITING_BEFORE_UNLOAD,
    STARTED,
    READY_TO_COMMIT,
    FAILED,
    FINISHED,
  };

  State state_ = INITIALIZATION;

  // The WebContents in which the navigation is taking place.
  // IMPORTANT: Because NavigationSimulator is used outside content/ where we
  // sometimes use WebContentsImpl and not TestWebContents, this cannot be
  // assumed to cast properly to TestWebContents.
  WebContentsImpl* web_contents_;

  // The renderer associated with this navigation.
  // Note: this can initially be null for browser-initiated navigations.
  TestRenderFrameHost* render_frame_host_;

  FrameTreeNode* frame_tree_node_;

  // The NavigationRequest associated with this navigation.
  NavigationRequest* request_;

  // Note: additional parameters to modify the navigation should be properly
  // initialized (if needed) in InitializeFromStartedRequest.
  GURL original_url_;
  GURL navigation_url_;
  net::IPEndPoint remote_endpoint_;
  bool is_signed_exchange_inner_response_ = false;
  std::string initial_method_;
  bool is_form_submission_ = false;
  bool browser_initiated_;
  bool same_document_ = false;
  Referrer referrer_;
  ui::PageTransition transition_;
  ReloadType reload_type_ = ReloadType::NONE;
  int session_history_offset_ = 0;
  bool has_user_gesture_ = true;
  service_manager::mojom::InterfaceProviderRequest interface_provider_request_;
  blink::mojom::DocumentInterfaceBrokerRequest
      document_interface_broker_content_request_;
  blink::mojom::DocumentInterfaceBrokerRequest
      document_interface_broker_blink_request_;
  std::string contents_mime_type_;
  CSPDisposition should_check_main_world_csp_ = CSPDisposition::CHECK;
  net::HttpResponseInfo::ConnectionInfo http_connection_info_ =
      net::HttpResponseInfo::CONNECTION_INFO_UNKNOWN;
  base::Optional<net::SSLInfo> ssl_info_;

  bool auto_advance_ = true;
  bool drop_swap_out_ack_ = false;
  bool block_on_before_unload_ack_ = false;

  // Generic params structure used for fully customized browser initiated
  // navigation requests. Only valid if explicitely provided.
  NavigationController::LoadURLParams* load_url_params_;

  bool history_list_was_cleared_ = false;
  bool should_replace_current_entry_ = false;
  base::Optional<bool> did_create_new_entry_;
  base::Optional<bool> intended_as_new_entry_;

  // These are used to sanity check the content/public/ API calls emitted as
  // part of the navigation.
  int num_did_start_navigation_called_ = 0;
  int num_will_start_request_called_ = 0;
  int num_will_redirect_request_called_ = 0;
  int num_will_fail_request_called_ = 0;
  int num_did_redirect_navigation_called_ = 0;
  int num_will_process_response_called_ = 0;
  int num_ready_to_commit_called_ = 0;
  int num_did_finish_navigation_called_ = 0;

  // Holds the last ThrottleCheckResult calculated by the navigation's
  // throttles. Will be unset before WillStartRequest is finished. Will be unset
  // while throttles are being run, but before they finish.
  base::Optional<NavigationThrottle::ThrottleCheckResult>
      last_throttle_check_result_;

  // GlobalRequestID for the associated NavigationHandle. Only valid after
  // WillProcessResponse has been invoked on the NavigationHandle.
  content::GlobalRequestID request_id_;

  // Closure that is set when MaybeWaitForThrottleChecksComplete is called.
  // Called in OnThrottleChecksComplete.
  base::OnceClosure throttle_checks_complete_closure_;

  // Closure that is called in OnThrottleChecksComplete if we are waiting on the
  // result. Calling this will quit the nested run loop.
  base::OnceClosure wait_closure_;

  // This member simply ensures that we do not disconnect
  // the NavigationClient interface, as it would be interpreted as a
  // cancellation coming from the renderer process side. This member interface
  // will never be bound.
  // Only used when PerNavigationMojoInterface is enabled.
  mojo::AssociatedInterfaceRequest<mojom::NavigationClient>
      navigation_client_request_;

  base::WeakPtrFactory<NavigationSimulatorImpl> weak_factory_;
};

}  // namespace content

#endif  // CONTENT_PUBLIC_TEST_NAVIGATION_SIMULATOR_H_

// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/test/ftl_signaling_playground.h"

#include <inttypes.h>

#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/guid.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/task/post_task.h"
#include "jingle/glue/thread_wrapper.h"
#include "remoting/base/chromium_url_request.h"
#include "remoting/base/logging.h"
#include "remoting/base/oauth_token_getter_impl.h"
#include "remoting/base/rsa_key_pair.h"
#include "remoting/base/service_urls.h"
#include "remoting/base/url_request_context_getter.h"
#include "remoting/protocol/auth_util.h"
#include "remoting/protocol/chromium_port_allocator_factory.h"
#include "remoting/protocol/jingle_session_manager.h"
#include "remoting/protocol/me2me_host_authenticator_factory.h"
#include "remoting/protocol/negotiating_client_authenticator.h"
#include "remoting/protocol/network_settings.h"
#include "remoting/protocol/pairing_registry.h"
#include "remoting/protocol/transport.h"
#include "remoting/protocol/transport_context.h"
#include "remoting/signaling/ftl_grpc_context.h"
#include "remoting/signaling/ftl_services.grpc.pb.h"
#include "remoting/signaling/ftl_signal_strategy.h"
#include "remoting/signaling/grpc_support/grpc_async_unary_request.h"
#include "remoting/test/cli_util.h"
#include "remoting/test/test_device_id_provider.h"
#include "remoting/test/test_oauth_token_getter.h"
#include "remoting/test/test_token_storage.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/transitional_url_loader_factory_owner.h"

namespace remoting {

namespace {

constexpr char kSwitchNameHelp[] = "help";
constexpr char kSwitchNameUsername[] = "username";
constexpr char kSwitchNameStoragePath[] = "storage-path";
constexpr char kSwitchNamePin[] = "pin";
constexpr char kSwitchNameHostId[] = "host-id";

const char* SignalStrategyErrorToString(SignalStrategy::Error error) {
  switch (error) {
    case SignalStrategy::OK:
      return "OK";
    case SignalStrategy::AUTHENTICATION_FAILED:
      return "AUTHENTICATION_FAILED";
    case SignalStrategy::NETWORK_ERROR:
      return "NETWORK_ERROR";
    case SignalStrategy::PROTOCOL_ERROR:
      return "PROTOCOL_ERROR";
  }
  return "";
}

class FakeTransportEventHandler final
    : public protocol::WebrtcTransport::EventHandler {
 public:
  explicit FakeTransportEventHandler(base::OnceClosure on_closed) {
    on_closed_ = std::move(on_closed);
  }

  ~FakeTransportEventHandler() override = default;

  // protocol::WebrtcTransport::EventHandler interface.
  void OnWebrtcTransportConnecting() override {
    HOST_LOG << "Webrtc transport is connecting...";
  }

  void OnWebrtcTransportConnected() override {
    HOST_LOG << "Webrtc transport is connected!!!";
    std::move(on_closed_).Run();
  }

  void OnWebrtcTransportError(protocol::ErrorCode error) override {
    LOG(ERROR) << "Webrtc transport error: " << error;
    std::move(on_closed_).Run();
  }

  void OnWebrtcTransportIncomingDataChannel(
      const std::string& name,
      std::unique_ptr<protocol::MessagePipe> pipe) override {}

  void OnWebrtcTransportMediaStreamAdded(
      scoped_refptr<webrtc::MediaStreamInterface> stream) override {}

  void OnWebrtcTransportMediaStreamRemoved(
      scoped_refptr<webrtc::MediaStreamInterface> stream) override {}

 private:
  base::OnceClosure on_closed_;
  DISALLOW_COPY_AND_ASSIGN(FakeTransportEventHandler);
};

}  // namespace

FtlSignalingPlayground::FtlSignalingPlayground() = default;

FtlSignalingPlayground::~FtlSignalingPlayground() = default;

bool FtlSignalingPlayground::ShouldPrintHelp() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(kSwitchNameHelp);
}

void FtlSignalingPlayground::PrintHelp() {
  printf(
      "Usage: %s [--auth-code=<auth-code>] [--host-id=<host-id>] [--pin=<pin>] "
      "[--storage-path=<storage-path>] [--username=<example@gmail.com>]\n",
      base::CommandLine::ForCurrentProcess()
          ->GetProgram()
          .MaybeAsASCII()
          .c_str());
}

void FtlSignalingPlayground::StartLoop() {
  jingle_glue::JingleThreadWrapper::EnsureForCurrentMessageLoop();

  base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  std::string username = cmd_line->GetSwitchValueASCII(kSwitchNameUsername);
  base::FilePath storage_path =
      cmd_line->GetSwitchValuePath(kSwitchNameStoragePath);

  storage_ = test::TestTokenStorage::OnDisk(username, storage_path);
  token_getter_ = std::make_unique<test::TestOAuthTokenGetter>(storage_.get());

  auto url_request_context_getter =
      base::MakeRefCounted<URLRequestContextGetter>(
          base::ThreadTaskRunnerHandle::Get());
  url_loader_factory_owner_ =
      std::make_unique<network::TransitionalURLLoaderFactoryOwner>(
          url_request_context_getter);

  base::RunLoop initialize_token_getter_loop;
  token_getter_->Initialize(initialize_token_getter_loop.QuitClosure());
  initialize_token_getter_loop.Run();

  std::vector<test::CommandOption> options{
      {"AcceptIncoming",
       base::BindRepeating(&FtlSignalingPlayground::AcceptIncoming,
                           base::Unretained(this))},
      {"ConnectToHost",
       base::BindRepeating(&FtlSignalingPlayground::ConnectToHost,
                           base::Unretained(this))}};

  test::RunCommandOptionsLoop(options);
}

void FtlSignalingPlayground::AcceptIncoming(base::OnceClosure on_done) {
  current_callback_ = std::move(on_done);

  SetUpSignaling();

  std::string host_id;
  auto* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(kSwitchNameHostId)) {
    host_id = cmd->GetSwitchValueASCII(kSwitchNameHostId);
  } else {
    host_id = base::GenerateGUID();
  }
  HOST_LOG << "Using host ID: " << host_id;

  std::string pin =
      test::ReadStringFromCommandLineOrStdin(kSwitchNamePin, "Pin: ");

  std::string pin_hash = protocol::GetSharedSecretHash(host_id, pin);

  auto key_pair = RsaKeyPair::Generate();
  std::string cert = key_pair->GenerateCertificate();
  auto factory = protocol::Me2MeHostAuthenticatorFactory::CreateWithPin(
      /* use_service_account */ false, storage_->FetchUserEmail(), cert,
      key_pair, /* domain_list */ {}, pin_hash, /* pairing_registry */ {});
  session_manager_->set_authenticator_factory(std::move(factory));
  HOST_LOG << "Waiting for incoming session...";
  session_manager_->AcceptIncoming(base::BindRepeating(
      &FtlSignalingPlayground::OnIncomingSession, base::Unretained(this)));
}

void FtlSignalingPlayground::OnIncomingSession(
    protocol::Session* owned_session,
    protocol::SessionManager::IncomingSessionResponse* response) {
  HOST_LOG << "Received incoming session!\n";
  RegisterSession(base::WrapUnique(owned_session),
                  protocol::TransportRole::CLIENT);
  *response = protocol::SessionManager::ACCEPT;
}

void FtlSignalingPlayground::ConnectToHost(base::OnceClosure on_done) {
  current_callback_ = std::move(on_done);
  on_signaling_connected_callback_ =
      base::BindOnce(&FtlSignalingPlayground::OnClientSignalingConnected,
                     base::Unretained(this));
  SetUpSignaling();
}

void FtlSignalingPlayground::OnClientSignalingConnected() {
  std::string host_id =
      test::ReadStringFromCommandLineOrStdin(kSwitchNameHostId, "Host ID: ");
  printf("Host JID: ");
  std::string host_jid = test::ReadString();

  protocol::ClientAuthenticationConfig client_auth_config;
  client_auth_config.host_id = host_id;
  client_auth_config.fetch_secret_callback = base::BindRepeating(
      &FtlSignalingPlayground::FetchSecret, base::Unretained(this));

  auto session = session_manager_->Connect(
      SignalingAddress(host_jid),
      std::make_unique<protocol::NegotiatingClientAuthenticator>(
          signal_strategy_->GetLocalAddress().id(), host_jid,
          client_auth_config));
  RegisterSession(std::move(session), protocol::TransportRole::SERVER);
}

void FtlSignalingPlayground::FetchSecret(
    bool pairing_supported,
    const protocol::SecretFetchedCallback& secret_fetched_callback) {
  std::string pin =
      test::ReadStringFromCommandLineOrStdin(kSwitchNamePin, "Pin: ");
  HOST_LOG << "Using PIN: " << pin;
  secret_fetched_callback.Run(pin);
}

void FtlSignalingPlayground::SetUpSignaling() {
  signal_strategy_ = std::make_unique<FtlSignalStrategy>(
      token_getter_.get(),
      std::make_unique<test::TestDeviceIdProvider>(storage_.get()));
  signal_strategy_->AddListener(this);

  session_manager_ =
      std::make_unique<protocol::JingleSessionManager>(signal_strategy_.get());
  auto protocol_config = protocol::CandidateSessionConfig::CreateDefault();
  protocol_config->set_webrtc_supported(true);
  session_manager_->set_protocol_config(std::move(protocol_config));

  signal_strategy_->Connect();
}

void FtlSignalingPlayground::TearDownSignaling() {
  on_signaling_connected_callback_.Reset();
  session_.reset();
  transport_.reset();
  transport_event_handler_.reset();
  signal_strategy_->RemoveListener(this);
  session_manager_.reset();
  signal_strategy_.reset();
}

void FtlSignalingPlayground::RegisterSession(
    std::unique_ptr<protocol::Session> session,
    protocol::TransportRole transport_role) {
  session_ = std::move(session);
  protocol::NetworkSettings network_settings(
      protocol::NetworkSettings::NAT_TRAVERSAL_FULL);
  auto transport_context = base::MakeRefCounted<protocol::TransportContext>(
      signal_strategy_.get(),
      std::make_unique<protocol::ChromiumPortAllocatorFactory>(),
      std::make_unique<ChromiumUrlRequestFactory>(
          url_loader_factory_owner_->GetURLLoaderFactory()),
      network_settings, transport_role);
  transport_context->set_ice_config_url(
      ServiceUrls::GetInstance()->ice_config_url(), token_getter_.get());
  std::unique_ptr<protocol::SessionManager> session_manager(
      new protocol::JingleSessionManager(signal_strategy_.get()));
  transport_event_handler_ = std::make_unique<FakeTransportEventHandler>(
      base::BindOnce(&FtlSignalingPlayground::AsyncTearDownAndRunCallback,
                     base::Unretained(this)));
  transport_ = std::make_unique<protocol::WebrtcTransport>(
      jingle_glue::JingleThreadWrapper::current(), transport_context,
      transport_event_handler_.get());
  session_->SetEventHandler(this);
  session_->SetTransport(transport_.get());
}

void FtlSignalingPlayground::OnSignalStrategyStateChange(
    SignalStrategy::State state) {
  if (state == SignalStrategy::CONNECTING) {
    HOST_LOG << "Connecting";
    return;
  }

  if (state == SignalStrategy::CONNECTED) {
    HOST_LOG << "Signaling connected. New JID: "
             << signal_strategy_->GetLocalAddress().jid();
    if (on_signaling_connected_callback_) {
      std::move(on_signaling_connected_callback_).Run();
    }
    return;
  }

  DCHECK(state == SignalStrategy::DISCONNECTED);

  auto error = signal_strategy_->GetError();

  TearDownSignaling();

  HOST_LOG << "Signaling disconnected. error="
           << SignalStrategyErrorToString(error);

  if (error == SignalStrategy::AUTHENTICATION_FAILED) {
    if (current_callback_) {
      token_getter_->ResetWithAuthenticationFlow(std::move(current_callback_));
    } else {
      token_getter_->InvalidateCache();
    }
    return;
  }

  if (current_callback_) {
    std::move(current_callback_).Run();
  }
}

bool FtlSignalingPlayground::OnSignalStrategyIncomingStanza(
    const jingle_xmpp::XmlElement* stanza) {
  return false;
}

void FtlSignalingPlayground::OnSessionStateChange(
    protocol::Session::State state) {
  HOST_LOG << "New session state: " << state;
  switch (state) {
    case protocol::Session::INITIALIZING:
    case protocol::Session::CONNECTING:
    case protocol::Session::ACCEPTING:
    case protocol::Session::ACCEPTED:
    case protocol::Session::AUTHENTICATING:
      // Don't care about these events.
      return;
    case protocol::Session::AUTHENTICATED:
      HOST_LOG << "Session is successfully authenticated!!!";
      return;

    case protocol::Session::CLOSED:
    case protocol::Session::FAILED:
      LOG(ERROR) << "Session failed/closed. Error: " << session_->error();
      break;
  }

  TearDownAndRunCallback();
}

void FtlSignalingPlayground::AsyncTearDownAndRunCallback() {
  tear_down_timer_.Start(FROM_HERE, base::TimeDelta(), this,
                         &FtlSignalingPlayground::TearDownAndRunCallback);
}

void FtlSignalingPlayground::TearDownAndRunCallback() {
  TearDownSignaling();
  if (current_callback_) {
    std::move(current_callback_).Run();
  }
}

}  // namespace remoting

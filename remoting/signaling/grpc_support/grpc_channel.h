// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef REMOTING_SIGNALING_GRPC_SUPPORT_GRPC_CHANNEL_H_
#define REMOTING_SIGNALING_GRPC_SUPPORT_GRPC_CHANNEL_H_

#include <memory>

#include "third_party/grpc/src/include/grpcpp/channel.h"

namespace remoting {

// TODO(yuweih): See if you should create a wrapper for the shared pointer.
#include "remoting/signaling/grpc_support/using_grpc_channel_shared_ptr.inc"

GrpcChannelSharedPtr CreateSslChannelForEndpoint(const std::string& endpoint);

}  // namespace remoting

#endif  // REMOTING_SIGNALING_GRPC_SUPPORT_GRPC_CHANNEL_H_

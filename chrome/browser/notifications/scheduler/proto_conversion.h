// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_PROTO_CONVERSION_H_
#define CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_PROTO_CONVERSION_H_

#include <memory>

#include "base/macros.h"
#include "chrome/browser/notifications/proto/client_state.pb.h"
#include "chrome/browser/notifications/proto/icon.pb.h"
#include "chrome/browser/notifications/scheduler/icon_entry.h"
#include "chrome/browser/notifications/scheduler/impression_types.h"

namespace notifications {

// Converts an icon entry to icon proto.
void IconEntryToProto(IconEntry* entry, notifications::proto::Icon* proto);

// Converts an icon proto to icon entry.
void IconEntryFromProto(proto::Icon* proto, notifications::IconEntry* entry);

// Converts client state to proto.
void ClientStateToProto(ClientState* client_state,
                        notifications::proto::ClientState* proto);

// Converts proto to client state.
void ClientStateFromProto(proto::ClientState* proto,
                          notifications::ClientState* client_state);

}  // namespace notifications

#endif  // CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_PROTO_CONVERSION_H_

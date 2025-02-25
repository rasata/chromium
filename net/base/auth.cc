// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/auth.h"

namespace net {

AuthChallengeInfo::AuthChallengeInfo() : is_proxy(false) {
}

AuthChallengeInfo::AuthChallengeInfo(const AuthChallengeInfo& other) = default;

bool AuthChallengeInfo::operator==(const AuthChallengeInfo& that) const {
  return (is_proxy == that.is_proxy && challenger == that.challenger &&
          scheme == that.scheme && realm == that.realm);
}

bool AuthChallengeInfo::operator!=(const AuthChallengeInfo& that) const {
  return !(*this == that);
}

AuthChallengeInfo::~AuthChallengeInfo() = default;

AuthCredentials::AuthCredentials() = default;

AuthCredentials::AuthCredentials(const base::string16& username,
                                 const base::string16& password)
    : username_(username),
      password_(password) {
}

AuthCredentials::~AuthCredentials() = default;

void AuthCredentials::Set(const base::string16& username,
                          const base::string16& password) {
  username_ = username;
  password_ = password;
}

bool AuthCredentials::Equals(const AuthCredentials& other) const {
  return username_ == other.username_ && password_ == other.password_;
}

bool AuthCredentials::Empty() const {
  return username_.empty() && password_.empty();
}

}  // namespace net

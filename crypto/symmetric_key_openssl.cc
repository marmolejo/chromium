// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crypto/symmetric_key.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/string_util.h"
#include "crypto/openssl_util.h"

namespace crypto {

SymmetricKey::~SymmetricKey() {
  std::fill(key_.begin(), key_.end(), '\0');  // Zero out the confidential key.
}

bool SymmetricKey::GetRawKey(std::string* raw_key) {
  *raw_key = key_;
  return true;
}

}  // namespace crypto

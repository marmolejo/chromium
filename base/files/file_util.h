// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains utility functions for dealing with the local
// filesystem.

#ifndef BASE_FILES_FILE_UTIL_H_
#define BASE_FILES_FILE_UTIL_H_

#include <unistd.h>
#include "base/base_export.h"
#include "base/posix/eintr_wrapper.h"

namespace base {

// Read exactly |bytes| bytes from file descriptor |fd|, storing the result
// in |buffer|. This function is protected against EINTR and partial reads.
// Returns true iff |bytes| bytes have been successfully read from |fd|.
BASE_EXPORT bool ReadFromFD(int fd, char* buffer, size_t bytes);

}  // namespace base

#endif  // BASE_FILES_FILE_UTIL_H_

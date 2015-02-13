// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_SOCKET_H_
#define NET_SOCKET_SOCKET_H_

#include "net/base/completion_callback.h"
#include "net/base/net_export.h"

namespace net {

class IOBuffer;

// Represents a read/write socket.
class NET_EXPORT Socket {
 public:
  virtual ~Socket() {}

  // Writes data, up to |buf_len| bytes, to the socket.  Note: data may be
  // written partially.  The number of bytes written is returned, or an error
  // is returned upon failure.  ERR_SOCKET_NOT_CONNECTED should be returned if
  // the socket is not currently connected.  The return value when the
  // connection is closed is undefined, and may be OS dependent.  ERR_IO_PENDING
  // is returned if the operation could not be completed synchronously, in which
  // case the result will be passed to the callback when available.  If the
  // operation is not completed immediately, the socket acquires a reference to
  // the provided buffer until the callback is invoked or the socket is
  // closed.  Implementations of this method should not modify the contents
  // of the actual buffer that is written to the socket.  If the socket is
  // Disconnected before the write completes, the callback will not be invoked.
  virtual int Write(IOBuffer* buf, int buf_len,
                    const CompletionCallback& callback) = 0;
};

}  // namespace net

#endif  // NET_SOCKET_SOCKET_H_

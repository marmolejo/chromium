// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_UDP_CLIENT_SOCKET_H_
#define NET_SOCKET_UDP_CLIENT_SOCKET_H_

#include "net/base/rand_callback.h"
#include "net/udp/datagram_client_socket.h"
#include "net/udp/udp_socket.h"

namespace net {

class BoundNetLog;

// A client socket that uses UDP as the transport layer.
class NET_EXPORT_PRIVATE UDPClientSocket : public DatagramClientSocket {
 public:
  UDPClientSocket(DatagramSocket::BindType bind_type,
                  const RandIntCallback& rand_int_cb);
  ~UDPClientSocket() override;

  // DatagramClientSocket implementation.
  int Connect(const IPEndPoint& address) override;
  int Write(IOBuffer* buf,
            int buf_len,
            const CompletionCallback& callback) override;
  void Close() override;

#if defined(OS_WIN)
  // Switch to use non-blocking IO. Must be called right after construction and
  // before other calls.
  void UseNonBlockingIO();
#endif

 private:
  UDPSocket socket_;
  DISALLOW_COPY_AND_ASSIGN(UDPClientSocket);
};

}  // namespace net

#endif  // NET_SOCKET_UDP_CLIENT_SOCKET_H_

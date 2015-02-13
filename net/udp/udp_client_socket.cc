// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/udp/udp_client_socket.h"

#include "net/base/net_errors.h"

namespace net {

UDPClientSocket::UDPClientSocket(DatagramSocket::BindType bind_type,
                                 const RandIntCallback& rand_int_cb)
    : socket_(bind_type, rand_int_cb) {
}

UDPClientSocket::~UDPClientSocket() {
}

int UDPClientSocket::Connect(const IPEndPoint& address) {
  int rv = socket_.Open(address.GetFamily());
  if (rv != OK)
    return rv;
  return socket_.Connect(address);
}

int UDPClientSocket::Write(IOBuffer* buf,
                          int buf_len,
                          const CompletionCallback& callback) {
  return socket_.Write(buf, buf_len, callback);
}

void UDPClientSocket::Close() {
  socket_.Close();
}

}  // namespace net

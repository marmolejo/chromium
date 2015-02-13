// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/udp/udp_socket_libevent.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "base/callback.h"
#include "base/logging.h"
#include "base/message_loop/message_loop.h"
#include "base/posix/eintr_wrapper.h"
#include "base/rand_util.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/net_util.h"
#include "net/socket/socket_descriptor.h"


namespace net {

namespace {

#if defined(OS_MACOSX)

// Returns IPv4 address in network order.
int GetIPv4AddressFromIndex(int socket, uint32 index, uint32* address){
  if (!index) {
    *address = htonl(INADDR_ANY);
    return OK;
  }
  ifreq ifr;
  ifr.ifr_addr.sa_family = AF_INET;
  if (!if_indextoname(index, ifr.ifr_name))
    return MapSystemError(errno);
  int rv = ioctl(socket, SIOCGIFADDR, &ifr);
  if (rv == -1)
    return MapSystemError(errno);
  *address = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr.s_addr;
  return OK;
}

#endif  // OS_MACOSX

}  // namespace

UDPSocketLibevent::UDPSocketLibevent(
    DatagramSocket::BindType bind_type,
    const RandIntCallback& rand_int_cb)
        : socket_(kInvalidSocket),
          addr_family_(0),
          is_connected_(false),
          socket_options_(SOCKET_OPTION_MULTICAST_LOOP),
          multicast_interface_(0),
          multicast_time_to_live_(1),
          bind_type_(bind_type),
          rand_int_cb_(rand_int_cb),
          read_watcher_(this),
          write_watcher_(this),
          read_buf_len_(0),
          recv_from_address_(NULL),
          write_buf_len_(0) {
  if (bind_type == DatagramSocket::RANDOM_BIND)
    DCHECK(!rand_int_cb.is_null());
}

UDPSocketLibevent::~UDPSocketLibevent() {
  Close();
}

int UDPSocketLibevent::Open(AddressFamily address_family) {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(socket_, kInvalidSocket);

  addr_family_ = ConvertAddressFamily(address_family);
  socket_ = CreatePlatformSocket(addr_family_, SOCK_DGRAM, 0);
  if (socket_ == kInvalidSocket)
    return -1;
  if (SetNonBlocking(socket_)) {
    const int err = -1;
    Close();
    return err;
  }
  return OK;
}

void UDPSocketLibevent::Close() {
  DCHECK(CalledOnValidThread());

  if (socket_ == kInvalidSocket)
    return;

  // Zero out any pending read/write callback state.
  read_buf_ = NULL;
  read_buf_len_ = 0;
  read_callback_.Reset();
  recv_from_address_ = NULL;
  write_buf_ = NULL;
  write_buf_len_ = 0;
  write_callback_.Reset();
  send_to_address_.reset();

  bool ok = read_socket_watcher_.StopWatchingFileDescriptor();
  DCHECK(ok);
  ok = write_socket_watcher_.StopWatchingFileDescriptor();
  DCHECK(ok);

  socket_ = kInvalidSocket;
  addr_family_ = 0;
  is_connected_ = false;
}

int UDPSocketLibevent::Write(IOBuffer* buf,
                             int buf_len,
                             const CompletionCallback& callback) {
  return SendToOrWrite(buf, buf_len, NULL, callback);
}

int UDPSocketLibevent::SendTo(IOBuffer* buf,
                              int buf_len,
                              const IPEndPoint& address,
                              const CompletionCallback& callback) {
  return SendToOrWrite(buf, buf_len, &address, callback);
}

int UDPSocketLibevent::SendToOrWrite(IOBuffer* buf,
                                     int buf_len,
                                     const IPEndPoint* address,
                                     const CompletionCallback& callback) {
  DCHECK(CalledOnValidThread());
  DCHECK_NE(kInvalidSocket, socket_);
  // We don't care currently about the completion
  // DCHECK(!callback.is_null());  // Synchronous operation not supported
  DCHECK_GT(buf_len, 0);

  int result = InternalSendTo(buf, buf_len, address);
    return result;
}

int UDPSocketLibevent::Connect(const IPEndPoint& address) {
  DCHECK_NE(socket_, kInvalidSocket);
  int rv = InternalConnect(address);
  is_connected_ = (rv == OK);
  return rv;
}

int UDPSocketLibevent::InternalConnect(const IPEndPoint& address) {
  DCHECK(CalledOnValidThread());
  DCHECK(!is_connected());
  DCHECK(!remote_address_.get());

  int rv = 0;

  SockaddrStorage storage;
  if (!address.ToSockAddr(storage.addr, &storage.addr_len))
    return ERR_ADDRESS_INVALID;

  rv = HANDLE_EINTR(connect(socket_, storage.addr, storage.addr_len));

  remote_address_.reset(new IPEndPoint(address));
  return rv;
}

void UDPSocketLibevent::ReadWatcher::OnFileCanReadWithoutBlocking(int) {
}

void UDPSocketLibevent::WriteWatcher::OnFileCanWriteWithoutBlocking(int) {
  if (!socket_->write_callback_.is_null())
    socket_->DidCompleteWrite();
}

void UDPSocketLibevent::DoReadCallback(int rv) {
  DCHECK_NE(rv, ERR_IO_PENDING);
  DCHECK(!read_callback_.is_null());

  // since Run may result in Read being called, clear read_callback_ up front.
  CompletionCallback c = read_callback_;
  read_callback_.Reset();
  c.Run(rv);
}

void UDPSocketLibevent::DoWriteCallback(int rv) {
  DCHECK_NE(rv, ERR_IO_PENDING);
  DCHECK(!write_callback_.is_null());

  // since Run may result in Write being called, clear write_callback_ up front.
  CompletionCallback c = write_callback_;
  write_callback_.Reset();
  c.Run(rv);
}

void UDPSocketLibevent::DidCompleteWrite() {
  int result =
      InternalSendTo(write_buf_.get(), write_buf_len_, send_to_address_.get());

  if (result != ERR_IO_PENDING) {
    write_buf_ = NULL;
    write_buf_len_ = 0;
    send_to_address_.reset();
    write_socket_watcher_.StopWatchingFileDescriptor();
    DoWriteCallback(result);
  }
}

void UDPSocketLibevent::LogWrite(int result,
                                 const char* bytes,
                                 const IPEndPoint* address) const {

}

int UDPSocketLibevent::InternalSendTo(IOBuffer* buf, int buf_len,
                                      const IPEndPoint* address) {
  SockaddrStorage storage;
  struct sockaddr* addr = storage.addr;
  if (!address) {
    addr = NULL;
    storage.addr_len = 0;
  } else {
    if (!address->ToSockAddr(storage.addr, &storage.addr_len)) {
      int result = ERR_ADDRESS_INVALID;
      LogWrite(result, NULL, NULL);
      return result;
    }
  }

  int result = HANDLE_EINTR(sendto(socket_,
                            buf->data(),
                            buf_len,
                            0,
                            addr,
                            storage.addr_len));
  if (result < 0)
    result = -1;
  if (result != ERR_IO_PENDING)
    LogWrite(result, buf->data(), address);
  return result;
}

int UDPSocketLibevent::SetMulticastInterface(uint32 interface_index) {
  DCHECK(CalledOnValidThread());
  if (is_connected())
    return ERR_SOCKET_IS_CONNECTED;
  multicast_interface_ = interface_index;
  return OK;
}

int UDPSocketLibevent::SetMulticastTimeToLive(int time_to_live) {
  DCHECK(CalledOnValidThread());
  if (is_connected())
    return ERR_SOCKET_IS_CONNECTED;

  if (time_to_live < 0 || time_to_live > 255)
    return ERR_INVALID_ARGUMENT;
  multicast_time_to_live_ = time_to_live;
  return OK;
}

int UDPSocketLibevent::SetMulticastLoopbackMode(bool loopback) {
  DCHECK(CalledOnValidThread());
  if (is_connected())
    return ERR_SOCKET_IS_CONNECTED;

  if (loopback)
    socket_options_ |= SOCKET_OPTION_MULTICAST_LOOP;
  else
    socket_options_ &= ~SOCKET_OPTION_MULTICAST_LOOP;
  return OK;
}

void UDPSocketLibevent::DetachFromThread() {
  base::NonThreadSafe::DetachFromThread();
}

}  // namespace net

// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/net_util.h"

#include <errno.h>
#include <string.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>

#include "build/build_config.h"

#if defined(OS_WIN)
#include <windows.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2bth.h>
#pragma comment(lib, "iphlpapi.lib")
#elif defined(OS_POSIX)
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#if !defined(OS_NACL)
#include <net/if.h>
#if !defined(OS_ANDROID)
#include <ifaddrs.h>
#endif  // !defined(OS_NACL)
#endif  // !defined(OS_ANDROID)
#endif  // defined(OS_POSIX)

#include "base/basictypes.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/sys_byteorder.h"
#include "url/url_canon.h"
#include "url/url_canon_ip.h"
#include "url/url_parse.h"

#if defined(OS_ANDROID)
#include "net/android/network_library.h"
#endif
#if defined(OS_WIN)
#include "net/base/winsock_init.h"
#endif

namespace net {

namespace {

// The general list of blocked ports. Will be blocked unless a specific
// protocol overrides it. (Ex: ftp can use ports 20 and 21)
static const int kRestrictedPorts[] = {
  1,    // tcpmux
  7,    // echo
  9,    // discard
  11,   // systat
  13,   // daytime
  15,   // netstat
  17,   // qotd
  19,   // chargen
  20,   // ftp data
  21,   // ftp access
  22,   // ssh
  23,   // telnet
  25,   // smtp
  37,   // time
  42,   // name
  43,   // nicname
  53,   // domain
  77,   // priv-rjs
  79,   // finger
  87,   // ttylink
  95,   // supdup
  101,  // hostriame
  102,  // iso-tsap
  103,  // gppitnp
  104,  // acr-nema
  109,  // pop2
  110,  // pop3
  111,  // sunrpc
  113,  // auth
  115,  // sftp
  117,  // uucp-path
  119,  // nntp
  123,  // NTP
  135,  // loc-srv /epmap
  139,  // netbios
  143,  // imap2
  179,  // BGP
  389,  // ldap
  465,  // smtp+ssl
  512,  // print / exec
  513,  // login
  514,  // shell
  515,  // printer
  526,  // tempo
  530,  // courier
  531,  // chat
  532,  // netnews
  540,  // uucp
  556,  // remotefs
  563,  // nntp+ssl
  587,  // stmp?
  601,  // ??
  636,  // ldap+ssl
  993,  // ldap+ssl
  995,  // pop3+ssl
  2049, // nfs
  3659, // apple-sasl / PasswordServer
  4045, // lockd
  6000, // X11
  6665, // Alternate IRC [Apple addition]
  6666, // Alternate IRC [Apple addition]
  6667, // Standard IRC [Apple addition]
  6668, // Alternate IRC [Apple addition]
  6669, // Alternate IRC [Apple addition]
  0xFFFF, // Used to block all invalid port numbers (see
          // third_party/WebKit/Source/platform/weborigin/KURL.cpp,
          // KURL::port())
};

// FTP overrides the following restricted ports.
static const int kAllowedFtpPorts[] = {
  21,   // ftp data
  22,   // ssh
};

bool IPNumberPrefixCheck(const IPAddressNumber& ip_number,
                         const unsigned char* ip_prefix,
                         size_t prefix_length_in_bits) {
  // Compare all the bytes that fall entirely within the prefix.
  int num_entire_bytes_in_prefix = prefix_length_in_bits / 8;
  for (int i = 0; i < num_entire_bytes_in_prefix; ++i) {
    if (ip_number[i] != ip_prefix[i])
      return false;
  }

  // In case the prefix was not a multiple of 8, there will be 1 byte
  // which is only partially masked.
  int remaining_bits = prefix_length_in_bits % 8;
  if (remaining_bits != 0) {
    unsigned char mask = 0xFF << (8 - remaining_bits);
    int i = num_entire_bytes_in_prefix;
    if ((ip_number[i] & mask) != (ip_prefix[i] & mask))
      return false;
  }
  return true;
}

}  // namespace

static base::LazyInstance<std::multiset<int> >::Leaky
    g_explicitly_allowed_ports = LAZY_INSTANCE_INITIALIZER;

size_t GetCountOfExplicitlyAllowedPorts() {
  return g_explicitly_allowed_ports.Get().size();
}

inline bool IsHostCharAlphanumeric(char c) {
  // We can just check lowercase because uppercase characters have already been
  // normalized.
  return ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9'));
}

bool IsCanonicalizedHostCompliant(const std::string& host) {
  if (host.empty())
    return false;

  bool in_component = false;
  bool most_recent_component_started_alphanumeric = false;
  bool last_char_was_underscore = false;

  for (std::string::const_iterator i(host.begin()); i != host.end(); ++i) {
    const char c = *i;
    if (!in_component) {
      most_recent_component_started_alphanumeric = IsHostCharAlphanumeric(c);
      if (!most_recent_component_started_alphanumeric && (c != '-'))
        return false;
      in_component = true;
    } else {
      if (c == '.') {
        if (last_char_was_underscore)
          return false;
        in_component = false;
      } else if (IsHostCharAlphanumeric(c) || (c == '-')) {
        last_char_was_underscore = false;
      } else if (c == '_') {
        last_char_was_underscore = true;
      } else {
        return false;
      }
    }
  }

  return most_recent_component_started_alphanumeric;
}

bool IsPortValid(int port) {
  return port >= 0 && port <= std::numeric_limits<uint16>::max();
}

bool IsPortAllowedByDefault(int port) {
  int array_size = arraysize(kRestrictedPorts);
  for (int i = 0; i < array_size; i++) {
    if (kRestrictedPorts[i] == port) {
      return false;
    }
  }
  return IsPortValid(port);
}

bool IsPortAllowedByFtp(int port) {
  int array_size = arraysize(kAllowedFtpPorts);
  for (int i = 0; i < array_size; i++) {
    if (kAllowedFtpPorts[i] == port) {
        return true;
    }
  }
  // Port not explicitly allowed by FTP, so return the default restrictions.
  return IsPortAllowedByDefault(port);
}

bool IsPortAllowedByOverride(int port) {
  if (g_explicitly_allowed_ports.Get().empty())
    return false;

  return g_explicitly_allowed_ports.Get().count(port) > 0;
}

int SetNonBlocking(int fd) {
#if defined(OS_WIN)
  unsigned long no_block = 1;
  return ioctlsocket(fd, FIONBIO, &no_block);
#elif defined(OS_POSIX)
  int flags = fcntl(fd, F_GETFL, 0);
  if (-1 == flags)
    return flags;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Don't compare IPv4 and IPv6 addresses (they have different range
// reservations). Keep separate reservation arrays for each IP type, and
// consolidate adjacent reserved ranges within a reservation array when
// possible.
// Sources for info:
// www.iana.org/assignments/ipv4-address-space/ipv4-address-space.xhtml
// www.iana.org/assignments/ipv6-address-space/ipv6-address-space.xhtml
// They're formatted here with the prefix as the last element. For example:
// 10.0.0.0/8 becomes 10,0,0,0,8 and fec0::/10 becomes 0xfe,0xc0,0,0,0...,10.
bool IsIPAddressReserved(const IPAddressNumber& host_addr) {
  static const unsigned char kReservedIPv4[][5] = {
      { 0,0,0,0,8 }, { 10,0,0,0,8 }, { 100,64,0,0,10 }, { 127,0,0,0,8 },
      { 169,254,0,0,16 }, { 172,16,0,0,12 }, { 192,0,2,0,24 },
      { 192,88,99,0,24 }, { 192,168,0,0,16 }, { 198,18,0,0,15 },
      { 198,51,100,0,24 }, { 203,0,113,0,24 }, { 224,0,0,0,3 }
  };
  static const unsigned char kReservedIPv6[][17] = {
      { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8 },
      { 0x40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2 },
      { 0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2 },
      { 0xc0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3 },
      { 0xe0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4 },
      { 0xf0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5 },
      { 0xf8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6 },
      { 0xfc,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7 },
      { 0xfe,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9 },
      { 0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10 },
      { 0xfe,0xc0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10 },
  };
  size_t array_size = 0;
  const unsigned char* array = NULL;
  switch (host_addr.size()) {
    case kIPv4AddressSize:
      array_size = arraysize(kReservedIPv4);
      array = kReservedIPv4[0];
      break;
    case kIPv6AddressSize:
      array_size = arraysize(kReservedIPv6);
      array = kReservedIPv6[0];
      break;
  }
  if (!array)
    return false;
  size_t width = host_addr.size() + 1;
  for (size_t i = 0; i < array_size; ++i, array += width) {
    if (IPNumberPrefixCheck(host_addr, array, array[width-1]))
      return true;
  }
  return false;
}

SockaddrStorage::SockaddrStorage(const SockaddrStorage& other)
    : addr_len(other.addr_len),
      addr(reinterpret_cast<struct sockaddr*>(&addr_storage)) {
  memcpy(addr, other.addr, addr_len);
}

void SockaddrStorage::operator=(const SockaddrStorage& other) {
  addr_len = other.addr_len;
  // addr is already set to &this->addr_storage by default ctor.
  memcpy(addr, other.addr, addr_len);
}

// Extracts the address and port portions of a sockaddr.
bool GetIPAddressFromSockAddr(const struct sockaddr* sock_addr,
                              socklen_t sock_addr_len,
                              const uint8** address,
                              size_t* address_len,
                              uint16* port) {
  if (sock_addr->sa_family == AF_INET) {
    if (sock_addr_len < static_cast<socklen_t>(sizeof(struct sockaddr_in)))
      return false;
    const struct sockaddr_in* addr =
        reinterpret_cast<const struct sockaddr_in*>(sock_addr);
    *address = reinterpret_cast<const uint8*>(&addr->sin_addr);
    *address_len = kIPv4AddressSize;
    if (port)
      *port = base::NetToHost16(addr->sin_port);
    return true;
  }

  if (sock_addr->sa_family == AF_INET6) {
    if (sock_addr_len < static_cast<socklen_t>(sizeof(struct sockaddr_in6)))
      return false;
    const struct sockaddr_in6* addr =
        reinterpret_cast<const struct sockaddr_in6*>(sock_addr);
    *address = reinterpret_cast<const uint8*>(&addr->sin6_addr);
    *address_len = kIPv6AddressSize;
    if (port)
      *port = base::NetToHost16(addr->sin6_port);
    return true;
  }

#if defined(OS_WIN)
  if (sock_addr->sa_family == AF_BTH) {
    if (sock_addr_len < static_cast<socklen_t>(sizeof(SOCKADDR_BTH)))
      return false;
    const SOCKADDR_BTH* addr =
        reinterpret_cast<const SOCKADDR_BTH*>(sock_addr);
    *address = reinterpret_cast<const uint8*>(&addr->btAddr);
    *address_len = kBluetoothAddressSize;
    if (port)
      *port = static_cast<uint16>(addr->port);
    return true;
  }
#endif

  return false;  // Unrecognized |sa_family|.
}

std::string IPAddressToPackedString(const IPAddressNumber& addr) {
  return std::string(reinterpret_cast<const char *>(&addr.front()),
                     addr.size());
}

std::string GetHostName() {
#if defined(OS_NACL)
  NOTIMPLEMENTED();
  return std::string();
#else  // defined(OS_NACL)
#if defined(OS_WIN)
  EnsureWinsockInit();
#endif

  // Host names are limited to 255 bytes.
  char buffer[256];
  int result = gethostname(buffer, sizeof(buffer));
  if (result != 0) {
    DVLOG(1) << "gethostname() failed with " << result;
    buffer[0] = '\0';
  }
  return std::string(buffer);
#endif  // !defined(OS_NACL)
}

ScopedPortException::ScopedPortException(int port) : port_(port) {
  g_explicitly_allowed_ports.Get().insert(port);
}

ScopedPortException::~ScopedPortException() {
  std::multiset<int>::iterator it =
      g_explicitly_allowed_ports.Get().find(port_);
  if (it != g_explicitly_allowed_ports.Get().end())
    g_explicitly_allowed_ports.Get().erase(it);
  else
    NOTREACHED();
}

bool HaveOnlyLoopbackAddresses() {
#if defined(OS_ANDROID)
  return android::HaveOnlyLoopbackAddresses();
#elif defined(OS_NACL)
  NOTIMPLEMENTED();
  return false;
#elif defined(OS_POSIX)
  struct ifaddrs* interface_addr = NULL;
  int rv = getifaddrs(&interface_addr);
  if (rv != 0) {
    DVLOG(1) << "getifaddrs() failed with errno = " << errno;
    return false;
  }

  bool result = true;
  for (struct ifaddrs* interface = interface_addr;
       interface != NULL;
       interface = interface->ifa_next) {
    if (!(IFF_UP & interface->ifa_flags))
      continue;
    if (IFF_LOOPBACK & interface->ifa_flags)
      continue;
    const struct sockaddr* addr = interface->ifa_addr;
    if (!addr)
      continue;
    if (addr->sa_family == AF_INET6) {
      // Safe cast since this is AF_INET6.
      const struct sockaddr_in6* addr_in6 =
          reinterpret_cast<const struct sockaddr_in6*>(addr);
      const struct in6_addr* sin6_addr = &addr_in6->sin6_addr;
      if (IN6_IS_ADDR_LOOPBACK(sin6_addr) || IN6_IS_ADDR_LINKLOCAL(sin6_addr))
        continue;
    }
    if (addr->sa_family != AF_INET6 && addr->sa_family != AF_INET)
      continue;

    result = false;
    break;
  }
  freeifaddrs(interface_addr);
  return result;
#elif defined(OS_WIN)
  // TODO(wtc): implement with the GetAdaptersAddresses function.
  NOTIMPLEMENTED();
  return false;
#else
  NOTIMPLEMENTED();
  return false;
#endif  // defined(various platforms)
}

AddressFamily GetAddressFamily(const IPAddressNumber& address) {
  switch (address.size()) {
    case kIPv4AddressSize:
      return ADDRESS_FAMILY_IPV4;
    case kIPv6AddressSize:
      return ADDRESS_FAMILY_IPV6;
    default:
      return ADDRESS_FAMILY_UNSPECIFIED;
  }
}

int ConvertAddressFamily(AddressFamily address_family) {
  switch (address_family) {
    case ADDRESS_FAMILY_UNSPECIFIED:
      return AF_UNSPEC;
    case ADDRESS_FAMILY_IPV4:
      return AF_INET;
    case ADDRESS_FAMILY_IPV6:
      return AF_INET6;
  }
  NOTREACHED();
  return AF_UNSPEC;
}

bool ParseURLHostnameToNumber(const std::string& hostname,
                              IPAddressNumber* ip_number) {
  // |hostname| is an already canoncalized hostname, conforming to RFC 3986.
  // For an IP address, this is defined in Section 3.2.2 of RFC 3986, with
  // the canonical form for IPv6 addresses defined in Section 4 of RFC 5952.
  url::Component host_comp(0, hostname.size());

  // If it has a bracket, try parsing it as an IPv6 address.
  if (hostname[0] == '[') {
    ip_number->resize(16);  // 128 bits.
    return url::IPv6AddressToNumber(
        hostname.data(), host_comp, &(*ip_number)[0]);
  }

  // Otherwise, try IPv4.
  ip_number->resize(4);  // 32 bits.
  int num_components;
  url::CanonHostInfo::Family family = url::IPv4AddressToNumber(
      hostname.data(), host_comp, &(*ip_number)[0], &num_components);
  return family == url::CanonHostInfo::IPV4;
}

bool ParseIPLiteralToNumber(const std::string& ip_literal,
                            IPAddressNumber* ip_number) {
  // |ip_literal| could be either a IPv4 or an IPv6 literal. If it contains
  // a colon however, it must be an IPv6 address.
  if (ip_literal.find(':') != std::string::npos) {
    // GURL expects IPv6 hostnames to be surrounded with brackets.
    std::string host_brackets = "[" + ip_literal + "]";
    url::Component host_comp(0, host_brackets.size());

    // Try parsing the hostname as an IPv6 literal.
    ip_number->resize(16);  // 128 bits.
    return url::IPv6AddressToNumber(host_brackets.data(), host_comp,
                                    &(*ip_number)[0]);
  }

  // Otherwise the string is an IPv4 address.
  ip_number->resize(4);  // 32 bits.
  url::Component host_comp(0, ip_literal.size());
  int num_components;
  url::CanonHostInfo::Family family = url::IPv4AddressToNumber(
      ip_literal.data(), host_comp, &(*ip_number)[0], &num_components);
  return family == url::CanonHostInfo::IPV4;
}

namespace {

const unsigned char kIPv4MappedPrefix[] =
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
}

IPAddressNumber ConvertIPv4NumberToIPv6Number(
    const IPAddressNumber& ipv4_number) {
  DCHECK(ipv4_number.size() == 4);

  // IPv4-mapped addresses are formed by:
  // <80 bits of zeros>  + <16 bits of ones> + <32-bit IPv4 address>.
  IPAddressNumber ipv6_number;
  ipv6_number.reserve(16);
  ipv6_number.insert(ipv6_number.end(),
                     kIPv4MappedPrefix,
                     kIPv4MappedPrefix + arraysize(kIPv4MappedPrefix));
  ipv6_number.insert(ipv6_number.end(), ipv4_number.begin(), ipv4_number.end());
  return ipv6_number;
}

bool IsIPv4Mapped(const IPAddressNumber& address) {
  if (address.size() != kIPv6AddressSize)
    return false;
  return std::equal(address.begin(),
                    address.begin() + arraysize(kIPv4MappedPrefix),
                    kIPv4MappedPrefix);
}

IPAddressNumber ConvertIPv4MappedToIPv4(const IPAddressNumber& address) {
  DCHECK(IsIPv4Mapped(address));
  return IPAddressNumber(address.begin() + arraysize(kIPv4MappedPrefix),
                         address.end());
}

bool IPNumberMatchesPrefix(const IPAddressNumber& ip_number,
                           const IPAddressNumber& ip_prefix,
                           size_t prefix_length_in_bits) {
  // Both the input IP address and the prefix IP address should be
  // either IPv4 or IPv6.
  DCHECK(ip_number.size() == 4 || ip_number.size() == 16);
  DCHECK(ip_prefix.size() == 4 || ip_prefix.size() == 16);

  DCHECK_LE(prefix_length_in_bits, ip_prefix.size() * 8);

  // In case we have an IPv6 / IPv4 mismatch, convert the IPv4 addresses to
  // IPv6 addresses in order to do the comparison.
  if (ip_number.size() != ip_prefix.size()) {
    if (ip_number.size() == 4) {
      return IPNumberMatchesPrefix(ConvertIPv4NumberToIPv6Number(ip_number),
                                   ip_prefix, prefix_length_in_bits);
    }
    return IPNumberMatchesPrefix(ip_number,
                                 ConvertIPv4NumberToIPv6Number(ip_prefix),
                                 96 + prefix_length_in_bits);
  }

  return IPNumberPrefixCheck(ip_number, &ip_prefix[0], prefix_length_in_bits);
}

const uint16* GetPortFieldFromSockaddr(const struct sockaddr* address,
                                       socklen_t address_len) {
  if (address->sa_family == AF_INET) {
    DCHECK_LE(sizeof(sockaddr_in), static_cast<size_t>(address_len));
    const struct sockaddr_in* sockaddr =
        reinterpret_cast<const struct sockaddr_in*>(address);
    return &sockaddr->sin_port;
  } else if (address->sa_family == AF_INET6) {
    DCHECK_LE(sizeof(sockaddr_in6), static_cast<size_t>(address_len));
    const struct sockaddr_in6* sockaddr =
        reinterpret_cast<const struct sockaddr_in6*>(address);
    return &sockaddr->sin6_port;
  } else {
    NOTREACHED();
    return NULL;
  }
}

int GetPortFromSockaddr(const struct sockaddr* address, socklen_t address_len) {
  const uint16* port_field = GetPortFieldFromSockaddr(address, address_len);
  if (!port_field)
    return -1;
  return base::NetToHost16(*port_field);
}

bool IsLocalhost(const std::string& host) {
  if (host == "localhost" ||
      host == "localhost.localdomain" ||
      host == "localhost6" ||
      host == "localhost6.localdomain6")
    return true;

  IPAddressNumber ip_number;
  if (ParseIPLiteralToNumber(host, &ip_number)) {
    size_t size = ip_number.size();
    switch (size) {
      case kIPv4AddressSize: {
        IPAddressNumber localhost_prefix;
        localhost_prefix.push_back(127);
        for (int i = 0; i < 3; ++i) {
          localhost_prefix.push_back(0);
        }
        return IPNumberMatchesPrefix(ip_number, localhost_prefix, 8);
      }

      case kIPv6AddressSize: {
        struct in6_addr sin6_addr;
        memcpy(&sin6_addr, &ip_number[0], kIPv6AddressSize);
        return !!IN6_IS_ADDR_LOOPBACK(&sin6_addr);
      }

      default:
        NOTREACHED();
    }
  }

  return false;
}

NetworkInterface::NetworkInterface()
    : type(NetworkChangeNotifier::CONNECTION_UNKNOWN), prefix_length(0) {
}

NetworkInterface::NetworkInterface(const std::string& name,
                                   const std::string& friendly_name,
                                   uint32 interface_index,
                                   NetworkChangeNotifier::ConnectionType type,
                                   const IPAddressNumber& address,
                                   uint32 prefix_length,
                                   int ip_address_attributes)
    : name(name),
      friendly_name(friendly_name),
      interface_index(interface_index),
      type(type),
      address(address),
      prefix_length(prefix_length),
      ip_address_attributes(ip_address_attributes) {
}

NetworkInterface::~NetworkInterface() {
}

unsigned CommonPrefixLength(const IPAddressNumber& a1,
                            const IPAddressNumber& a2) {
  DCHECK_EQ(a1.size(), a2.size());
  for (size_t i = 0; i < a1.size(); ++i) {
    unsigned diff = a1[i] ^ a2[i];
    if (!diff)
      continue;
    for (unsigned j = 0; j < CHAR_BIT; ++j) {
      if (diff & (1 << (CHAR_BIT - 1)))
        return i * CHAR_BIT + j;
      diff <<= 1;
    }
    NOTREACHED();
  }
  return a1.size() * CHAR_BIT;
}

unsigned MaskPrefixLength(const IPAddressNumber& mask) {
  IPAddressNumber all_ones(mask.size(), 0xFF);
  return CommonPrefixLength(mask, all_ones);
}

ScopedWifiOptions::~ScopedWifiOptions() {
}

}  // namespace net

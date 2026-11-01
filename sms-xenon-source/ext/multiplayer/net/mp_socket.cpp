// UDP sockets for the multiplayer extension (mp_socket.h).
#include "mp_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define MP_BAD_SOCKET INVALID_SOCKET
static int lastError() { return WSAGetLastError(); }
static void closeSocket(sock_t s) { closesocket(s); }
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define MP_BAD_SOCKET (-1)
static int lastError() { return errno; }
static void closeSocket(sock_t s) { ::close(s); }
#endif

namespace mp {

bool socketsInit()
{
#ifdef _WIN32
	static bool done, ok;
	if (!done) {
		WSADATA data;
		ok   = WSAStartup(MAKEWORD(2, 2), &data) == 0;
		done = true;
	}
	return ok;
#else
	return true;
#endif
}

static std::string errorText(const char* what)
{
	char buf[160];
	snprintf(buf, sizeof buf, "%s failed (error %d)", what, lastError());
	return buf;
}

bool Address::operator==(const Address& o) const
{
	if (length != o.length)
		return false;
	const sockaddr* a = (const sockaddr*)storage;
	const sockaddr* b = (const sockaddr*)o.storage;
	if (a->sa_family != b->sa_family)
		return false;
	if (a->sa_family == AF_INET) {
		const sockaddr_in* x = (const sockaddr_in*)a;
		const sockaddr_in* y = (const sockaddr_in*)b;
		return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
	}
	if (a->sa_family == AF_INET6) {
		const sockaddr_in6* x = (const sockaddr_in6*)a;
		const sockaddr_in6* y = (const sockaddr_in6*)b;
		return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr);
	}
	return !memcmp(storage, o.storage, length);
}

std::string Address::text() const
{
	// getnameinfo, not inet_ntop: every Windows toolchain declares it
	char host[NI_MAXHOST] = "?";
	char out[NI_MAXHOST + 16];
	const sockaddr* a = (const sockaddr*)storage;
	if (!length || getnameinfo(a, (socklen_t)length, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0)
		return "?";
	unsigned port = 0;
	if (a->sa_family == AF_INET)
		port = ntohs(((const sockaddr_in*)a)->sin_port);
	else if (a->sa_family == AF_INET6)
		port = ntohs(((const sockaddr_in6*)a)->sin6_port);
	// an IPv4 player on a dual-stack host reads as plain IPv4
	const char* shown = !strncmp(host, "::ffff:", 7) && strchr(host + 7, '.') ? host + 7 : host;
	if (a->sa_family == AF_INET6 && shown == host)
		snprintf(out, sizeof out, "[%s]:%u", host, port);
	else
		snprintf(out, sizeof out, "%s:%u", shown, port);
	return out;
}

Socket::~Socket() { close(); }

bool Socket::isOpen() const { return fd_ != (intptr_t)MP_BAD_SOCKET; }

void Socket::close()
{
	if (isOpen())
		closeSocket((sock_t)fd_);
	fd_ = (intptr_t)MP_BAD_SOCKET;
}

static bool nonBlocking(sock_t s)
{
#ifdef _WIN32
	u_long on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool Socket::open(uint16_t port, std::string& error, bool loopbackOnly)
{
	close();
	if (!socketsInit()) {
		error = "Networking could not start";
		return false;
	}
	// IPv6 with IPv4 mapped in, so one socket hears both
	sock_t s = loopbackOnly ? MP_BAD_SOCKET : socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (s != MP_BAD_SOCKET) {
		int off = 0;
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off);
		sockaddr_in6 a;
		memset(&a, 0, sizeof a);
		a.sin6_family = AF_INET6;
		a.sin6_addr   = in6addr_any;
		a.sin6_port   = htons(port);
		if (bind(s, (const sockaddr*)&a, sizeof a) == 0 && nonBlocking(s)) {
			fd_     = (intptr_t)s;
			family_ = AF_INET6;
			return true;
		}
		closeSocket(s);
	}
	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == MP_BAD_SOCKET) {
		error = errorText("Opening a network socket");
		return false;
	}
	sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family      = AF_INET;
	a.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
	a.sin_port        = htons(port);
	if (bind(s, (const sockaddr*)&a, sizeof a) != 0) {
		char buf[160];
		snprintf(buf, sizeof buf, "Port %u is already in use, or not allowed", (unsigned)port);
		error = buf;
		closeSocket(s);
		return false;
	}
	if (!nonBlocking(s)) {
		error = errorText("Setting up the socket");
		closeSocket(s);
		return false;
	}
	fd_     = (intptr_t)s;
	family_ = AF_INET;
	return true;
}

bool Socket::openFor(const Address& to, std::string& error)
{
	close();
	if (!socketsInit()) {
		error = "Networking could not start";
		return false;
	}
	const int family = ((const sockaddr*)to.storage)->sa_family;
	sock_t s         = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (s == MP_BAD_SOCKET || !nonBlocking(s)) {
		error = errorText("Opening a network socket");
		if (s != MP_BAD_SOCKET)
			closeSocket(s);
		return false;
	}
	// a host on this computer (the launcher passing packets on): listen on
	// this computer only, not on every network (unbound, the first send
	// binds it to any address)
	if (family == AF_INET && (ntohl(((const sockaddr_in*)to.storage)->sin_addr.s_addr) >> 24) == 127) {
		sockaddr_in a;
		memset(&a, 0, sizeof a);
		a.sin_family      = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(s, (const sockaddr*)&a, sizeof a);
	} else if (family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((const sockaddr_in6*)to.storage)->sin6_addr)) {
		sockaddr_in6 a;
		memset(&a, 0, sizeof a);
		a.sin6_family = AF_INET6;
		a.sin6_addr   = in6addr_loopback;
		bind(s, (const sockaddr*)&a, sizeof a);
	}
	fd_     = (intptr_t)s;
	family_ = family;
	return true;
}

uint16_t Socket::localPort() const
{
	sockaddr_storage a;
	socklen_t len = sizeof a;
	if (!isOpen() || getsockname((sock_t)fd_, (sockaddr*)&a, &len) != 0)
		return 0;
	if (a.ss_family == AF_INET)
		return ntohs(((sockaddr_in*)&a)->sin_port);
	return ntohs(((sockaddr_in6*)&a)->sin6_port);
}

bool Socket::wait(int ms)
{
	if (!isOpen())
		return false;
	fd_set set;
	FD_ZERO(&set);
	FD_SET((sock_t)fd_, &set);
	timeval tv;
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return select((int)fd_ + 1, &set, nullptr, nullptr, &tv) > 0;
}

int Socket::recv(void* buf, size_t size, Address& from)
{
	if (!isOpen())
		return -1;
	for (;;) {
		socklen_t len = sizeof from.storage;
		int n = (int)recvfrom((sock_t)fd_, (char*)buf, (int)size, 0, (sockaddr*)from.storage, &len);
		if (n >= 0) {
			from.length = (uint32_t)len;
			return n;
		}
#ifdef _WIN32
		// a port-unreachable reply to an earlier send; not a packet, keep reading
		if (WSAGetLastError() == WSAECONNRESET)
			continue;
#endif
		return -1;
	}
}

bool Socket::send(const void* buf, size_t size, const Address& to)
{
	if (!isOpen())
		return false;
	Address dest = to;
	// an IPv4 address through the dual-stack socket goes as ::ffff:a.b.c.d
	if (family_ == AF_INET6 && ((const sockaddr*)to.storage)->sa_family == AF_INET) {
		const sockaddr_in* v4 = (const sockaddr_in*)to.storage;
		sockaddr_in6 m;
		memset(&m, 0, sizeof m);
		m.sin6_family = AF_INET6;
		m.sin6_port   = v4->sin_port;
		uint8_t* b    = (uint8_t*)&m.sin6_addr;
		b[10] = b[11] = 0xff;
		memcpy(b + 12, &v4->sin_addr, 4);
		memcpy(dest.storage, &m, sizeof m);
		dest.length = sizeof m;
	}
	return sendto((sock_t)fd_, (const char*)buf, (int)size, 0, (const sockaddr*)dest.storage,
	              (socklen_t)dest.length)
	       == (int)size;
}

bool Socket::resolve(const char* text, uint16_t defaultPort, Address& out, std::string& error)
{
	if (!socketsInit()) {
		error = "Networking could not start";
		return false;
	}
	std::string host = text ? text : "";
	std::string port = std::to_string(defaultPort);
	// "[v6]:port", "host:port", or a bare IPv6 address (more than one colon)
	if (!host.empty() && host[0] == '[') {
		size_t end = host.find(']');
		if (end == std::string::npos) {
			error = "The address has a [ without a ]";
			return false;
		}
		if (end + 1 < host.size() && host[end + 1] == ':')
			port = host.substr(end + 2);
		host = host.substr(1, end - 1);
	} else {
		size_t colon = host.find(':');
		if (colon != std::string::npos && host.find(':', colon + 1) == std::string::npos) {
			port = host.substr(colon + 1);
			host = host.substr(0, colon);
		}
	}
	while (!host.empty() && (host.back() == ' ' || host.back() == '\t'))
		host.pop_back();
	while (!host.empty() && (host[0] == ' ' || host[0] == '\t'))
		host.erase(0, 1);
	if (host.empty()) {
		error = "No address to join was given";
		return false;
	}
	char* end   = nullptr;
	long number = strtol(port.c_str(), &end, 10);
	if (port.empty() || *end || number < 1 || number > 65535) {
		error = "The port must be a number from 1 to 65535";
		return false;
	}
	addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	addrinfo* res     = nullptr;
	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
		error = "Could not find the address " + host;
		return false;
	}
	// IPv4 first: home routers forward IPv4 ports
	const addrinfo* pick = res;
	for (const addrinfo* r = res; r; r = r->ai_next)
		if (r->ai_family == AF_INET) {
			pick = r;
			break;
		}
	memset(out.storage, 0, sizeof out.storage);
	memcpy(out.storage, pick->ai_addr, pick->ai_addrlen);
	out.length = (uint32_t)pick->ai_addrlen;
	freeaddrinfo(res);
	return true;
}

} // namespace mp

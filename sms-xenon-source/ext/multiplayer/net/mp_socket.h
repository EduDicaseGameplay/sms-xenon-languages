#ifndef SMS_MP_SOCKET_H
#define SMS_MP_SOCKET_H

// One non-blocking UDP socket, IPv6 dual-stack where the system has it (so a
// host takes IPv4 and IPv6 players on one port), IPv4 otherwise.

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace mp {

struct Address {
	uint8_t storage[128]; // a sockaddr_storage
	uint32_t length = 0;
	bool operator==(const Address& o) const;
	bool operator!=(const Address& o) const { return !(*this == o); }
	std::string text() const; // "1.2.3.4:24002" or "[::1]:24002"
};

class Socket {
public:
	Socket() = default;
	~Socket();
	Socket(const Socket&)            = delete;
	Socket& operator=(const Socket&) = delete;

	// Listens on `port` (0: any free port); false with `error` set on failure.
	// `loopbackOnly`: heard from this computer only (the launcher passes the
	// other players' packets on), so no firewall asks about it.
	bool open(uint16_t port, std::string& error, bool loopbackOnly = false);
	// For joining: a socket on any port that can reach `to` (on this computer
	// only, when `to` is).
	bool openFor(const Address& to, std::string& error);
	void close();
	bool isOpen() const;
	uint16_t localPort() const;

	// Waits up to `ms` for a packet to arrive.
	bool wait(int ms);
	// One packet, or -1 when there is none.
	int recv(void* buf, size_t size, Address& from);
	bool send(const void* buf, size_t size, const Address& to);

	// "host", "host:port", "[v6]:port", "v6": false with `error` set when it
	// does not resolve. A missing port is `defaultPort`.
	static bool resolve(const char* text, uint16_t defaultPort, Address& out, std::string& error);

private:
	intptr_t fd_ = -1;
	int family_  = 0;
};

// Networking needs starting once on Windows; a no-op elsewhere.
bool socketsInit();

} // namespace mp

#endif

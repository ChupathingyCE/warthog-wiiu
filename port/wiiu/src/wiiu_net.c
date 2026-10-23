/*
WIIU_NET.C

The platform layer's sockets and process helpers (port/linux/src/posix.h,
in place of posix_net.c) on the Wii U: nsysnet's BSD sockets through wut,
the console's address from nn::ac, random numbers, and the user's secret on
the SD card.

The game's Winsock addresses (sockaddr_in as the Xbox laid it out: a 16-bit
family, the port and the address in network order) have the layout of
nsysnet's own, field for field, on this big-endian machine, so they are
passed through as they are.

The network comes up as the owner's Butter and Jelly brings it up on the
Wii U (its src/platform/http_curl.cpp, Http::Init, lines 306 to 338; and
src/platform/wiiu/platform_wiiu.cpp:184 for the address), once, before the
first socket that goes anywhere (wiiu_network_start):

- wut's start-up has already started the socket library (socket_lib_init)
  and asked nn::ac to connect without waiting (ACConnectAsync: wut's
  wut_socket_common.c, __init_wut_socket, linked with the first socket
  call), as for Butter and Jelly.
- ACInitialize (http_curl.cpp:309).
- ACIsApplicationConnected; only if the console is not connected yet, its
  start-up configuration (ACGetStartupId) connected with
  ACConnectWithConfigId, which can block for seconds on a cold association
  (http_curl.cpp:315-327; Aroma usually has the link up already).
- ACGetAssignedAddress: the console's address, in this machine's order
  (http_curl.cpp:330, platform_wiiu.cpp:184), logged by its kind only.

Every socket call that fails is logged (the call, the socket, the option or
the destination's kind, errno and nsysnet's own code), and each socket's
life: below, "the log of the sockets".
*/

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nn/ac.h>
#include <nsysnet/_socket.h>

#include "posix.h"
#include "monocypher.h"

#include "wiiu_port.h"
#include "ifaddrs.h"
#include "net/if.h"

/* Winsock error codes (winsockx.h) */
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEACCES 10013
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEDESTADDRREQ 10039
#define WSAEMSGSIZE 10040
#define WSAEPROTOTYPE 10041
#define WSAENOPROTOOPT 10042
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN 10050
#define WSAENETUNREACH 10051
#define WSAENETRESET 10052
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAESHUTDOWN 10058
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAEHOSTDOWN 10064
#define WSAEHOSTUNREACH 10065

/* (newlib's, outside its Linux extensions: wut's map of nsysnet's codes
gives it, wut_socket_common.c) */
#ifndef ESHUTDOWN
#define ESHUTDOWN 110
#endif

/* Winsock SOL_SOCKET option values (winsockx.h) */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SO_REUSEADDR 0x0004
#define WINSOCK_SO_KEEPALIVE 0x0008
#define WINSOCK_SO_BROADCAST 0x0020
#define WINSOCK_SO_LINGER 0x0080
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002
#define WINSOCK_SO_ERROR 0x1007
#define WINSOCK_SO_TYPE 0x1008

/* (each thread's: no thread-local storage in an RPX) */
void *platform_thread_local(void **key, unsigned long size);
static void *last_error_key;
#define last_error (*(int *)platform_thread_local(&last_error_key, sizeof(int)))

/* ---------- the log of the sockets

Every socket call that fails is logged, with the call, its socket, what it
was asked (the option, the size, the destination's kind) and the error as
the C library gave it (errno), as nsysnet gave it (socketlasterr: wut maps
it to errno, several of nsysnet's to EINVAL) and as the game is given it
(Winsock's). Calls that would only block are not failures. Each call and
error is logged its first few times, then once every so many.

Each socket's life is logged too, without an address (only its kind:
loopback, private, public, broadcast): made, bound (the port), the options
it is given, the first datagram or bytes out to each kind of address and the
first in from each, and at its close what it carried. That is internet
play's tunnel step by step (p2p.c: its socket, the first probe to the host,
the host's first answer), with the tunnel's own lines below
(p2p_trace_packet, p2p_trace_datagram) for the handover to the game. */

#define TRACED_SOCKETS 64
/* (the first sockets of a session are the ones worth reading: the game's,
the tunnel's, the brokers'; later ones log failures only) */
#define LOGGED_SOCKETS 64

enum
{
	_kind_any,
	_kind_loopback,
	_kind_private,
	_kind_shared,
	_kind_link_local,
	_kind_broadcast,
	_kind_multicast,
	_kind_public,
	_kind_count
};

static const char *const kind_names[_kind_count] =
{
	"any address", "the loopback", "a private address", "a shared (carrier-grade NAT) address",
	"a link-local address", "the broadcast address", "a multicast address", "a public address",
};

/* (network order in) */
static int kind_of(uint32_t network_address)
{
	uint32_t address = ntohl(network_address);

	if (!address)
		return _kind_any;
	if ((address >> 24) == 127)
		return _kind_loopback;
	if (address == 0xFFFFFFFFUL)
		return _kind_broadcast;
	if ((address >> 28) == 0xE)
		return _kind_multicast;
	if ((address >> 24) == 10 || (address >> 20) == 0xAC1 || (address >> 16) == 0xC0A8)
		return _kind_private;
	if ((address >> 22) == (0x64400000UL >> 22))
		return _kind_shared;
	if ((address >> 16) == 0xA9FE)
		return _kind_link_local;
	return _kind_public;
}

static const char *address_kind_of(const void *address, int length)
{
	const struct sockaddr_in *in = address;

	if (!address || length < (int)sizeof(*in))
		return "no address";
	if (in->sin_family != AF_INET)
		return "not an IPv4 address";
	return kind_names[kind_of(in->sin_addr.s_addr)];
}

static struct socket_trace
{
	unsigned char used;
	unsigned char logged;
	unsigned char stream;
	unsigned char sent_kinds_logged;
	unsigned char received_kinds_logged;
	unsigned char options_logged;
	unsigned short port;
	unsigned long sent, sent_bytes, send_failures, received, received_bytes;
} socket_traces[TRACED_SOCKETS];
static int sockets_made;

static struct socket_trace *trace_of(int socket)
{
	return socket >= 0 && socket < TRACED_SOCKETS && socket_traces[socket].used ? &socket_traces[socket] : NULL;
}

static const char *errno_name(int error)
{
	switch (error)
	{
	case EINVAL: return "EINVAL";
	case EBADF: return "EBADF";
	case EACCES: return "EACCES";
	case EPERM: return "EPERM";
	case EFAULT: return "EFAULT";
	case EMFILE: return "EMFILE";
	case ENFILE: return "ENFILE";
	case EAGAIN: return "EAGAIN";
	case EINPROGRESS: return "EINPROGRESS";
	case EALREADY: return "EALREADY";
	case ENOTSOCK: return "ENOTSOCK";
	case EDESTADDRREQ: return "EDESTADDRREQ";
	case EMSGSIZE: return "EMSGSIZE";
	case EPROTOTYPE: return "EPROTOTYPE";
	case ENOPROTOOPT: return "ENOPROTOOPT";
	case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
	case EOPNOTSUPP: return "EOPNOTSUPP";
	case EAFNOSUPPORT: return "EAFNOSUPPORT";
	case EADDRINUSE: return "EADDRINUSE";
	case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
	case ENETDOWN: return "ENETDOWN";
	case ENETUNREACH: return "ENETUNREACH";
	case ENETRESET: return "ENETRESET";
	case ECONNABORTED: return "ECONNABORTED";
	case ECONNRESET: return "ECONNRESET";
	case EPIPE: return "EPIPE";
	case ENOBUFS: return "ENOBUFS";
	case ENOMEM: return "ENOMEM";
	case EISCONN: return "EISCONN";
	case ENOTCONN: return "ENOTCONN";
	case ESHUTDOWN: return "ESHUTDOWN";
	case ETIMEDOUT: return "ETIMEDOUT";
	case ECONNREFUSED: return "ECONNREFUSED";
	case EHOSTUNREACH: return "EHOSTUNREACH";
	case EBUSY: return "EBUSY";
	case EIO: return "EIO";
	case ENODEV: return "ENODEV";
	case ECANCELED: return "ECANCELED";
	case ENOTTY: return "ENOTTY";
	default: return error >= 10000 ? "nsysnet's own (errno 10000 + its code)" : "another";
	}
}

static int winsock_error_for(int error)
{
	switch (error)
	{
	case EINTR: return WSAEINTR;
	case EBADF: return WSAEBADF;
	case EACCES: case EPERM: return WSAEACCES;
	case EFAULT: return WSAEFAULT;
	case EMFILE: case ENFILE: return WSAEMFILE;
	case EAGAIN: return WSAEWOULDBLOCK;
	case EINPROGRESS: return WSAEINPROGRESS;
	case EALREADY: return WSAEALREADY;
	case ENOTSOCK: return WSAENOTSOCK;
	case EDESTADDRREQ: return WSAEDESTADDRREQ;
	case EMSGSIZE: return WSAEMSGSIZE;
	case EPROTOTYPE: return WSAEPROTOTYPE;
	case ENOPROTOOPT: return WSAENOPROTOOPT;
	case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
	case EOPNOTSUPP: return WSAEOPNOTSUPP;
	case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
	case EADDRINUSE: return WSAEADDRINUSE;
	case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
	case ENETDOWN: return WSAENETDOWN;
	case ENETUNREACH: return WSAENETUNREACH;
	case ENETRESET: return WSAENETRESET;
	case ECONNABORTED: return WSAECONNABORTED;
	case ECONNRESET: case EPIPE: return WSAECONNRESET;
	case ENOBUFS: case ENOMEM: return WSAENOBUFS;
	case EISCONN: return WSAEISCONN;
	case ENOTCONN: return WSAENOTCONN;
	case ESHUTDOWN: return WSAESHUTDOWN;
	case ETIMEDOUT: return WSAETIMEDOUT;
	case ECONNREFUSED: return WSAECONNREFUSED;
	case EHOSTUNREACH: return WSAEHOSTUNREACH;
	default: return WSAEINVAL;
	}
}

/* each call and error's count, for the log's limit */
#define FAILURE_KINDS 48
#define FAILURES_LOGGED 4
#define FAILURES_THEN_EVERY 200
static struct
{
	const char *call;
	int error;
	unsigned long count;
} failure_counts[FAILURE_KINDS];

static unsigned long failure_count(const char *call, int error)
{
	int index;

	for (index = 0; index < FAILURE_KINDS; index++)
	{
		if (!failure_counts[index].call)
		{
			failure_counts[index].call = call;
			failure_counts[index].error = error;
		}
		if (failure_counts[index].call == call && failure_counts[index].error == error)
			return ++failure_counts[index].count;
	}
	return 1;
}

/* the call failed: errno and nsysnet's own code taken at once, the Winsock
error set, the failure logged; -1 */
static int fail_call(const char *call, int socket, const char *detail)
{
	int error = errno;
	int nsysnet = RPLWRAP(socketlasterr)();
	unsigned long count;

	last_error = winsock_error_for(error);
	if (last_error == WSAEWOULDBLOCK || last_error == WSAEINPROGRESS || last_error == WSAEALREADY)
		return -1;
	count = failure_count(call, error);
	if (count <= FAILURES_LOGGED || count % FAILURES_THEN_EVERY == 0)
	{
		wiiu_log("net: %s on socket %d%s%s failed: errno %d (%s), nsysnet %d, Winsock %d%s", call, socket,
			detail && *detail ? ", " : "", detail ? detail : "", error, errno_name(error), nsysnet, last_error,
			count > FAILURES_LOGGED ? " (and as often before)" : "");
	}
	return -1;
}

static int succeed_call(int result, const char *call, int socket, const char *detail)
{
	if (result < 0)
		return fail_call(call, socket, detail);
	last_error = 0;
	return result;
}

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	int result = socket(family, type, protocol);
	char detail[48];

	snprintf(detail, sizeof(detail), "family %d, type %d, protocol %d", family, type, protocol);
	if (result < 0)
		return fail_call("socket", -1, detail);
	last_error = 0;
	if (result >= 0 && result < TRACED_SOCKETS)
	{
		struct socket_trace *trace = &socket_traces[result];

		memset(trace, 0, sizeof(*trace));
		trace->used = 1;
		trace->stream = type == SOCK_STREAM;
		trace->logged = ++sockets_made <= LOGGED_SOCKETS;
		if (trace->logged)
			wiiu_log("net: socket %d made (%s)", result, trace->stream ? "TCP" : type == SOCK_DGRAM ? "UDP" : detail);
	}
	return result;
}

int posix_socket_close(int socket)
{
	struct socket_trace *trace = trace_of(socket);

	if (trace)
	{
		if (trace->logged && (trace->sent || trace->received || trace->send_failures))
		{
			wiiu_log("net: socket %d closed: %lu out (%lu bytes, %lu failed), %lu in (%lu bytes)", socket, trace->sent,
				trace->sent_bytes, trace->send_failures, trace->received, trace->received_bytes);
		}
		trace->used = 0;
	}
	return succeed_call(close(socket), "close", socket, NULL);
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	const struct sockaddr_in *in = address;
	struct socket_trace *trace = trace_of(socket);
	char detail[64];
	int result;

	snprintf(detail, sizeof(detail), "%s, port %u", address_kind_of(address, address_length),
		address && address_length >= (int)sizeof(*in) ? (unsigned)ntohs(in->sin_port) : 0u);
	result = bind(socket, address, (socklen_t)address_length);
	if (result < 0)
		return fail_call("bind", socket, detail);
	last_error = 0;
	if (trace && trace->logged)
	{
		struct sockaddr_in bound;
		socklen_t length = sizeof(bound);

		if (getsockname(socket, (struct sockaddr *)&bound, &length) == 0)
		{
			trace->port = bound.sin_port;
			wiiu_log("net: socket %d bound to %s, port %u", socket, detail, (unsigned)ntohs(bound.sin_port));
		}
		else
		{
			wiiu_log("net: socket %d bound to %s (getsockname then failed: errno %d)", socket, detail, errno);
		}
	}
	return result;
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	const struct sockaddr_in *in = address;
	struct socket_trace *trace = trace_of(socket);
	char detail[64];
	int result = connect(socket, address, (socklen_t)address_length);

	snprintf(detail, sizeof(detail), "to %s, port %u", address_kind_of(address, address_length),
		address && address_length >= (int)sizeof(*in) ? (unsigned)ntohs(in->sin_port) : 0u);
	if (trace && trace->logged)
		wiiu_log("net: socket %d connecting %s", socket, detail);
	if (result < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK))
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	return succeed_call(result, "connect", socket, detail);
}

int posix_socket_listen(int socket, int backlog)
{
	return succeed_call(listen(socket, backlog), "listen", socket, NULL);
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	socklen_t length = address_length ? (socklen_t)*address_length : 0;
	int result = accept(socket, address, address_length ? &length : NULL);

	if (address_length)
		*address_length = (int)length;
	return succeed_call(result, "accept", socket, NULL);
}

/* the first datagram or bytes out to each kind of address, and each that
failed (the error is fail_call's) */
static void trace_sent(int socket, const void *address, int address_length, int length, int result)
{
	struct socket_trace *trace = trace_of(socket);
	const struct sockaddr_in *in = address;
	int kind;

	if (!trace)
		return;
	if (result < 0)
	{
		if (last_error != WSAEWOULDBLOCK)
			trace->send_failures++;
		return;
	}
	trace->sent++;
	trace->sent_bytes += (unsigned long)result;
	kind = in && address_length >= (int)sizeof(*in) && in->sin_family == AF_INET ? kind_of(in->sin_addr.s_addr) : -1;
	if (trace->logged && kind >= 0 && !(trace->sent_kinds_logged & (1u << kind)))
	{
		trace->sent_kinds_logged |= (unsigned char)(1u << kind);
		wiiu_log("net: socket %d (port %u): first datagram out to %s, port %u (%d of %d bytes)", socket,
			(unsigned)ntohs(trace->port), kind_names[kind], (unsigned)ntohs(in->sin_port), result, length);
	}
	else if (trace->logged && kind < 0 && trace->sent == 1)
	{
		wiiu_log("net: socket %d: first bytes out (%d of %d)", socket, result, length);
	}
}

static void trace_received(int socket, const void *address, int address_length, int result)
{
	struct socket_trace *trace = trace_of(socket);
	const struct sockaddr_in *in = address;
	int kind;

	if (!trace || result < 0)
		return;
	trace->received++;
	trace->received_bytes += (unsigned long)result;
	kind = in && address_length >= (int)sizeof(*in) && in->sin_family == AF_INET ? kind_of(in->sin_addr.s_addr) : -1;
	if (trace->logged && kind >= 0 && !(trace->received_kinds_logged & (1u << kind)))
	{
		trace->received_kinds_logged |= (unsigned char)(1u << kind);
		wiiu_log("net: socket %d (port %u): first datagram in from %s, port %u (%d bytes)", socket,
			(unsigned)ntohs(trace->port), kind_names[kind], (unsigned)ntohs(in->sin_port), result);
	}
	else if (trace->logged && kind < 0 && trace->received == 1)
	{
		wiiu_log("net: socket %d: first bytes in (%d)", socket, result);
	}
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	char detail[40];
	int result;

	snprintf(detail, sizeof(detail), "%d bytes, flags 0x%x", length, (unsigned)flags);
	result = succeed_call((int)send(socket, buffer, (size_t)length, flags), "send", socket, detail);
	trace_sent(socket, NULL, 0, length, result);
	return result;
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	const struct sockaddr_in *in = address;
	char detail[96];
	int result = (int)sendto(socket, buffer, (size_t)length, flags, address, (socklen_t)address_length);

	if (result < 0 && errno == EISCONN)
		result = (int)send(socket, buffer, (size_t)length, flags);
	if (result < 0)
	{
		int error = errno;

		snprintf(detail, sizeof(detail), "%d bytes to %s, port %u, flags 0x%x", length,
			address_kind_of(address, address_length),
			in && address_length >= (int)sizeof(*in) ? (unsigned)ntohs(in->sin_port) : 0u, (unsigned)flags);
		errno = error;
		fail_call("sendto", socket, detail);
	}
	else
	{
		last_error = 0;
	}
	trace_sent(socket, address, address_length, length, result);
	return result;
}

/* ---------- a datagram socket's receive length

The console refuses a datagram socket's receive whose length is over some
limit with EMSGSIZE (nsysnet 12), whatever the datagram's own size: on the
console the game's 1200 bytes and the network test's 512 were taken, and
internet play's tunnel's 2048 refused on every receive, so the tunnel never
read the host's answers (port.txt of 2026-10-22, "recvfrom on socket 13
failed: errno 122 (EMSGSIZE)"). Linux and Windows take any length.

So a receive refused that way is asked again at once with the next shorter
of these lengths, and the longest taken is the limit from then on, logged
once. The tunnel's packets are at most 1431 bytes (p2p.c's
MAXIMUM_PACKET_SIZE), the game's datagrams 1200 (network_connection.h). Every datagram the protocols send fits an Ethernet frame (at
most 1472 bytes of UDP). A receive refused even at the shortest is
Winsock's WSAEMSGSIZE: a datagram dropped, as Winsock drops one longer than
the buffer. */

static const int receive_lengths[] = { 1472, 1440, 1432, 1400, 1280, 1200, 1024, 512 };
/* the longest length taken; 0 until a receive is refused */
static int receive_limit;

static int datagram_socket(int socket)
{
	struct socket_trace *trace = trace_of(socket);

	return trace ? !trace->stream : 1;
}

/* one receive: recv when address is NULL, else recvfrom */
static int receive_once(int socket, void *buffer, int length, int flags, void *address, socklen_t *address_length)
{
	if (address)
		return (int)recvfrom(socket, buffer, (size_t)length, flags, address, address_length);
	return (int)recv(socket, buffer, (size_t)length, flags);
}

static int receive(const char *call, int socket, void *buffer, int length, int flags, void *address,
	int *address_length)
{
	socklen_t socket_length = address && address_length ? (socklen_t)*address_length : 0;
	socklen_t *lengths = address && address_length ? &socket_length : NULL;
	void *from = address && address_length ? address : NULL;
	int datagram = datagram_socket(socket);
	int asked = length, result, step;

	if (datagram && receive_limit && length > receive_limit)
		length = receive_limit;
	result = receive_once(socket, buffer, length, flags, from, lengths);
	for (step = 0; result < 0 && errno == EMSGSIZE && datagram &&
		step < (int)(sizeof(receive_lengths) / sizeof(*receive_lengths)); step++)
	{
		if (receive_lengths[step] >= length)
			continue;
		length = receive_lengths[step];
		if (lengths)
			socket_length = (socklen_t)*address_length;
		result = receive_once(socket, buffer, length, flags, from, lengths);
		if (result >= 0 || errno != EMSGSIZE)
		{
			if (!receive_limit || length < receive_limit)
			{
				receive_limit = length;
				wiiu_log("net: a datagram socket's receive of %d bytes refused (EMSGSIZE); receiving at most %d "
					"from now on", asked, length);
			}
			break;
		}
	}
	if (address_length)
		*address_length = (int)socket_length;
	return succeed_call(result, call, socket, NULL);
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	int result = receive("recv", socket, buffer, length, flags, NULL, NULL);

	trace_received(socket, NULL, 0, result);
	return result;
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	int result = receive("recvfrom", socket, buffer, length, flags, address, address_length);

	trace_received(socket, address, address_length ? *address_length : 0, result);
	return result;
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed_call(shutdown(socket, how), "shutdown", socket, NULL);
}

/* (SO_NONBLOCK, as Butter and Jelly's discovery.cpp SetNonBlocking does it
on the Wii U, and wut's fcntl and FIONBIO do underneath) */
int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int value = nonblocking ? 1 : 0;

	return succeed_call(setsockopt(socket, SOL_SOCKET, SO_NONBLOCK, &value, sizeof(value)), "setsockopt",
		socket, nonblocking ? "SO_NONBLOCK 1" : "SO_NONBLOCK 0");
}

int posix_socket_set_nodelay(int socket)
{
	int value = 1;

	return succeed_call(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)), "setsockopt",
		socket, "TCP_NODELAY 1");
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result = ioctl(socket, FIONREAD, &available);

	if (result >= 0)
		*count = (posix_ulong)available;
	return succeed_call(result, "ioctl", socket, "FIONREAD");
}

static int translate_option(int level, int name, int *host_level, int *host_name)
{
	if (level != WINSOCK_SOL_SOCKET)
	{
		*host_level = level;
		*host_name = name;
		return 0;
	}
	*host_level = SOL_SOCKET;
	switch (name)
	{
	case WINSOCK_SO_REUSEADDR: *host_name = SO_REUSEADDR; return 0;
	case WINSOCK_SO_KEEPALIVE: *host_name = SO_KEEPALIVE; return 0;
	case WINSOCK_SO_BROADCAST: *host_name = SO_BROADCAST; return 0;
	case WINSOCK_SO_LINGER: *host_name = SO_LINGER; return 0;
	case WINSOCK_SO_SNDBUF: *host_name = SO_SNDBUF; return 0;
	case WINSOCK_SO_RCVBUF: *host_name = SO_RCVBUF; return 0;
	case WINSOCK_SO_ERROR: *host_name = SO_ERROR; return 0;
	case WINSOCK_SO_TYPE: *host_name = SO_TYPE; return 0;
	default: return -1;
	}
}

static const char *option_name(int level, int name)
{
	if (level == SOL_SOCKET)
	{
		switch (name)
		{
		case SO_REUSEADDR: return "SO_REUSEADDR";
		case SO_KEEPALIVE: return "SO_KEEPALIVE";
		case SO_BROADCAST: return "SO_BROADCAST";
		case SO_LINGER: return "SO_LINGER";
		case SO_SNDBUF: return "SO_SNDBUF";
		case SO_RCVBUF: return "SO_RCVBUF";
		case SO_ERROR: return "SO_ERROR";
		case SO_TYPE: return "SO_TYPE";
		case SO_NONBLOCK: return "SO_NONBLOCK";
		case SO_WINSCALE: return "SO_WINSCALE";
		case SO_RUSRBUF: return "SO_RUSRBUF";
		}
		return "a SOL_SOCKET option";
	}
	if (level == IPPROTO_TCP && name == TCP_NODELAY)
		return "TCP_NODELAY";
	return "an option";
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	struct socket_trace *trace = trace_of(socket);
	int host_level, host_name, number = 0, result;
	char detail[96];

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		/* Xbox-only options such as SO_ENCRYPT have nothing to do here */
		last_error = 0;
		return 0;
	}
	if (value && length == (int)sizeof(int))
		memcpy(&number, value, sizeof(number));
	snprintf(detail, sizeof(detail), "%s (level %d, option 0x%x) %d, %d bytes", option_name(host_level, host_name),
		host_level, (unsigned)host_name, number, length);
	if (host_level == SOL_SOCKET && (host_name == SO_SNDBUF || host_name == SO_RCVBUF))
	{
		/* the largest size the console takes, up to the one asked for. The
		game and internet play's tunnel ask for 1 MB on every socket
		(transport_endpoint_winsock.c, p2p.c), which the console refuses
		with EINVAL (nsysnet 11; its own sizes are 1492 bytes to send and
		4096 to receive: port.txt of 2026-10-22), more than all the memory
		an application can give its network for its sockets' buffers (wut's
		nn/nets2/somemopt.h: 128 KB to 3 MB). Butter and Jelly asks for
		256 KB for its one stream (http_curl.cpp, ConfigureSocket);
		Moonlight steps a refused size down until one is taken
		(moonlight-common-c's PlatformSockets.c, bindUdpSocket). The game
		has several sockets, so 64 KB at most. The socket's first ask is
		logged with the size taken; none taken keeps the system's, without
		an error, as Linux takes a size above its limit */
		static const int sizes[] = { 65536, 32768, 16384, 8192 };
		int size = number, index, taken = 0, current = 0;
		socklen_t current_length = sizeof(current);
		int logged = trace && trace->logged && !(trace->options_logged & (host_name == SO_SNDBUF ? 1 : 2));

		if (trace)
			trace->options_logged |= (unsigned char)(host_name == SO_SNDBUF ? 1 : 2);
		for (index = -1; index < (int)(sizeof(sizes) / sizeof(*sizes)) && !taken; index++)
		{
			if (index >= 0)
			{
				if (sizes[index] >= size)
					continue;
				size = sizes[index];
			}
			if (size <= 0)
				break;
			taken = setsockopt(socket, SOL_SOCKET, host_name, &size, sizeof(size)) == 0;
		}
		if (getsockopt(socket, SOL_SOCKET, host_name, &current, &current_length) != 0)
			current = -1;
		if (logged && taken)
		{
			wiiu_log("net: socket %d: %s %d asked for, %d taken (it reads %d)", socket,
				option_name(host_level, host_name), number, size, current);
		}
		else if (logged)
		{
			wiiu_log("net: socket %d: %s %d asked for, none down to %d taken: the system's size kept (%d bytes)",
				socket, option_name(host_level, host_name), number, sizes[sizeof(sizes) / sizeof(*sizes) - 1],
				current);
		}
		last_error = 0;
		return 0;
	}
	if (host_level == SOL_SOCKET && host_name == SO_BROADCAST && length == (int)sizeof(int))
	{
		/* (on or off: the game asks with -1, Butter and Jelly's discovery.cpp
		with 1) */
		int on = number != 0;

		result = setsockopt(socket, host_level, host_name, &on, sizeof(on));
	}
	else
	{
		result = setsockopt(socket, host_level, host_name, value, (socklen_t)length);
	}
	if (result < 0)
		return fail_call("setsockopt", socket, detail);
	last_error = 0;
	if (trace && trace->logged)
		wiiu_log("net: socket %d: %s set", socket, detail);
	return result;
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	int host_level, host_name;
	socklen_t socket_length = (socklen_t)*length;
	char detail[64];
	int result;

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	result = getsockopt(socket, host_level, host_name, value, &socket_length);
	*length = (int)socket_length;
	snprintf(detail, sizeof(detail), "%s (level %d, option 0x%x)", option_name(host_level, host_name), host_level,
		(unsigned)host_name);
	return succeed_call(result, "getsockopt", socket, detail);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	socklen_t length = address_length ? (socklen_t)*address_length : 0;
	int result = getsockname(socket, address, address_length ? &length : NULL);

	if (address_length)
		*address_length = (int)length;
	return succeed_call(result, "getsockname", socket, NULL);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	socklen_t length = address_length ? (socklen_t)*address_length : 0;
	int result = getpeername(socket, address, address_length ? &length : NULL);

	if (address_length)
		*address_length = (int)length;
	return succeed_call(result, "getpeername", socket, NULL);
}

/* Winsock's select over lists of sockets, by poll: read for data, the end
or an error, write for room or an error, error for a connect that failed.
(wut's poll is nsysnet's select underneath, whose sets hold sockets 0 to 31:
wut_socket's poll.c) */
int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	static const short events[3] = { POLLIN, POLLOUT, POLLPRI };
	static const short ready[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI | POLLERR };
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	struct pollfd *descriptors;
	long long milliseconds = (long long)timeout_seconds * 1000 + ((long long)timeout_microseconds + 999) / 1000;
	int total = 0;
	int list, index;
	int result;

	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			lists[list] = NULL;
		else
			total += *counts[list];
	}
	descriptors = total ? malloc(sizeof(*descriptors) * (size_t)total) : NULL;
	if (total && !descriptors)
	{
		errno = ENOMEM;
		return fail_call("select", -1, "no memory for the list");
	}
	total = 0;
	for (list = 0; list < 3; list++)
	{
		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			descriptors[total].fd = lists[list][index];
			descriptors[total].events = events[list];
			descriptors[total].revents = 0;
		}
	}
	if (total)
	{
		result = poll(descriptors, (nfds_t)total, infinite ? -1 :
			(int)(milliseconds < 0 ? 0 : milliseconds > INT_MAX ? INT_MAX : milliseconds));
	}
	else
	{
		/* (nothing to wait on: a sleep) */
		if (!infinite && milliseconds > 0)
			OSSleepTicks(OSMillisecondsToTicks(milliseconds));
		result = 0;
	}
	if (result < 0)
	{
		char detail[96];
		int highest = -1, failure = errno;

		for (index = 0; index < total; index++)
		{
			if (descriptors[index].fd > highest)
				highest = descriptors[index].fd;
		}
		snprintf(detail, sizeof(detail), "%d sockets (%d read, %d write), the highest %d, %lld ms", total,
			lists[0] ? *counts[0] : 0, lists[1] ? *counts[1] : 0, highest, infinite ? -1LL : milliseconds);
		free(descriptors);
		errno = failure;
		return fail_call("select", -1, detail);
	}
	result = 0;
	total = 0;
	for (list = 0; list < 3; list++)
	{
		int kept = 0;

		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			int descriptor = lists[list][index];
			int pending = 0;
			socklen_t length = sizeof(pending);

			if (!(descriptors[total].revents & ready[list]))
				continue;
			/* (as Winsock: writeable means connected; a failed connect is
			not writeable) */
			if (list == 1 && getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending)
			{
				errno = pending;
				fail_call("connect (found by select)", descriptor, NULL);
				continue;
			}
			lists[list][kept++] = descriptor;
		}
		if (lists[list])
			*counts[list] = kept;
		result += kept;
	}
	free(descriptors);
	if (result > 0)
		last_error = 0;
	return result;
}

/* ---------- internet play's tunnel, to the game (p2p.c, P2P_TRACE_DATAGRAMS)

What became of the datagrams the host sends the game through the tunnel:
the handover from the tunnel to the game's own connection. As the original
Xbox logs them (warthog-xbox's port/xbox/src/xbox_p2p.c): each outcome its
first four times, then every 10 seconds; and the packets the tunnel opened,
by type, every 10 seconds for the first two minutes, then every minute. */

void p2p_trace_datagram(unsigned short source_port, unsigned short port, int size, int result)
{
	static const char *const outcomes[] =
	{
		"no stand-in for it",
		"not a port of the game's",
		"not sent to the game",
		"sent to the game",
	};
	static struct
	{
		long count;
		long logged;
		OSTime time;
	} traced[4];
	int outcome = result + 2;
	OSTime now = OSGetSystemTime();

	if (outcome < 0 || outcome > 3)
		return;
	traced[outcome].count++;
	if (traced[outcome].logged < 4 || OSTicksToMilliseconds(now - traced[outcome].time) >= 10000)
	{
		wiiu_log("tunnel: %ld datagram(s) from the host's port %u for the game's port %u (%d bytes): %s",
			traced[outcome].count, (unsigned)ntohs(source_port), (unsigned)ntohs(port), size, outcomes[outcome]);
		traced[outcome].count = 0;
		traced[outcome].logged++;
		traced[outcome].time = now;
	}
}

void p2p_trace_packet(int type, int size)
{
	static long counts[8];
	static long bytes, total;
	static OSTime time, first;
	OSTime now = OSGetSystemTime();

	if (!total++)
	{
		first = time = now;
		wiiu_log("tunnel: the first packet from a peer opened (type %d, %d bytes)", type, size);
	}
	counts[type >= 0 && type < 8 ? type : 7]++;
	bytes += size;
	if (OSTicksToMilliseconds(now - time) >= (OSTicksToMilliseconds(now - first) < 120000 ? 10000 : 60000))
	{
		wiiu_log("tunnel: in the last %lld s, from peers: types 0-7 %ld %ld %ld %ld %ld %ld %ld %ld (%ld bytes)",
			(long long)OSTicksToMilliseconds(now - time) / 1000, counts[0], counts[1], counts[2], counts[3], counts[4],
			counts[5], counts[6], counts[7], bytes);
		memset(counts, 0, sizeof(counts));
		bytes = 0;
		time = now;
	}
}

/* ---------- the network (nn::ac), and the console's address */

/* (its kind, never the address: private, carrier-grade shared, other) */
static const char *address_kind(uint32_t address)
{
	if ((address >> 24) == 10 || (address >> 20) == 0xAC1 || (address >> 16) == 0xC0A8)
		return "private";
	if ((address >> 22) == (0x64400000UL >> 22))
		return "shared (carrier-grade NAT)";
	if ((address >> 16) == 0xA9FE)
		return "link-local";
	return "public";
}

/* 0 not up, 1 up; the first caller brings it up, any other waits for it.
A failure is tried again, no sooner than this after the last */
#define NETWORK_RETRY_MS 10000
static int network_state;
static OSTime network_failed_at;
static OSMutex network_mutex;
static int network_mutex_ready;

int wiiu_network_start(void)
{
	BOOL connected = FALSE;
	ACConfigId configuration = 0;
	uint32_t address = 0;
	OSTime start;

	if (__atomic_load_n(&network_state, __ATOMIC_ACQUIRE))
		return network_state > 0;
	/* (the mutex made once: the game's thread and the list's may both ask
	at the start) */
	if (!__atomic_exchange_n(&network_mutex_ready, 1, __ATOMIC_ACQ_REL))
	{
		OSInitMutex(&network_mutex);
		__atomic_store_n(&network_mutex_ready, 2, __ATOMIC_RELEASE);
	}
	while (__atomic_load_n(&network_mutex_ready, __ATOMIC_ACQUIRE) != 2)
		OSYieldThread();
	OSLockMutex(&network_mutex);
	if (network_state || (network_failed_at &&
		OSTicksToMilliseconds(OSGetSystemTime() - network_failed_at) < NETWORK_RETRY_MS))
	{
		OSUnlockMutex(&network_mutex);
		return network_state > 0;
	}
	start = OSGetSystemTime();
	if (!NNResult_IsSuccess(ACInitialize()))
	{
		wiiu_log("network: ACInitialize failed");
		network_failed_at = OSGetSystemTime();
		OSUnlockMutex(&network_mutex);
		return 0;
	}
	if (!NNResult_IsSuccess(ACIsApplicationConnected(&connected)) || !connected)
	{
		wiiu_log("network: not connected yet; connecting with the console's start-up configuration");
		if (!NNResult_IsSuccess(ACGetStartupId(&configuration)))
		{
			wiiu_log("network: no network configuration on this console (System Settings, Internet)");
			network_failed_at = OSGetSystemTime();
			OSUnlockMutex(&network_mutex);
			return 0;
		}
		if (!NNResult_IsSuccess(ACConnectWithConfigId(configuration)))
		{
			wiiu_log("network: could not connect with configuration %u (%lld ms)", (unsigned)configuration,
				(long long)OSTicksToMilliseconds(OSGetSystemTime() - start));
			network_failed_at = OSGetSystemTime();
			OSUnlockMutex(&network_mutex);
			return 0;
		}
	}
	if (NNResult_IsSuccess(ACGetAssignedAddress(&address)) && address)
	{
		wiiu_log("network: up%s, a %s address (%lld ms)", connected ? " (already connected)" : "",
			address_kind(address), (long long)OSTicksToMilliseconds(OSGetSystemTime() - start));
	}
	else
	{
		wiiu_log("network: connected, but no address yet (%lld ms)",
			(long long)OSTicksToMilliseconds(OSGetSystemTime() - start));
	}
	__atomic_store_n(&network_state, 1, __ATOMIC_RELEASE);
	OSUnlockMutex(&network_mutex);
	return 1;
}

static int network_ready(void)
{
	return wiiu_network_start();
}

/* the console's address, in network order; 0 if it has none. wut has no
call for the network's mask: system link broadcasts to the limited
broadcast address (a mask of 0, below), which every machine on the local
network hears */
static unsigned int console_address(unsigned int *mask)
{
	uint32_t address = 0;

	if (!network_ready() || !NNResult_IsSuccess(ACGetAssignedAddress(&address)) || !address)
		return 0;
	if (mask)
		*mask = 0;
	return htonl(address);
}

posix_ulong posix_local_ipv4_address(void)
{
	return console_address(NULL);
}

/* (ifaddrs.h: the one interface, for system link's broadcast) */
int getifaddrs(struct ifaddrs **addresses)
{
	struct entry
	{
		struct ifaddrs entry;
		struct sockaddr_in address, mask, broadcast;
		char name[8];
	} *entry;
	unsigned int mask = 0;
	unsigned int address = console_address(&mask);

	*addresses = NULL;
	if (!address)
	{
		errno = ENETDOWN;
		return -1;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
	{
		errno = ENOMEM;
		return -1;
	}
	strcpy(entry->name, "wl0");
	entry->address.sin_family = AF_INET;
	entry->address.sin_addr.s_addr = address;
	entry->mask.sin_family = AF_INET;
	entry->mask.sin_addr.s_addr = mask;
	entry->broadcast.sin_family = AF_INET;
	entry->broadcast.sin_addr.s_addr = address | ~mask;
	entry->entry.ifa_name = entry->name;
	entry->entry.ifa_flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
	entry->entry.ifa_addr = (struct sockaddr *)&entry->address;
	entry->entry.ifa_netmask = (struct sockaddr *)&entry->mask;
	entry->entry.ifa_broadaddr = (struct sockaddr *)&entry->broadcast;
	*addresses = &entry->entry;
	return 0;
}

void freeifaddrs(struct ifaddrs *addresses)
{
	free(addresses);
}

/* ---------- random numbers

Cafe OS gives an application no random number source of its own. These
come from the timing jitter of the console's clock across thread switches
and the time, hashed with BLAKE2b together with a seed kept on the SD card
(rewritten each start), and a running state mixed into every request. */

#define SEED_PATH "fs:/vol/external01/wiiu/apps/halo/random.seed"

static unsigned char random_state[64];
static int random_ready;

static void random_gather(crypto_blake2b_ctx *context)
{
	int sample;

	for (sample = 0; sample < 256; sample++)
	{
		OSTick before = OSGetSystemTick();
		OSTime time;

		OSYieldThread();
		time = OSGetSystemTime();
		before ^= OSGetSystemTick() << 7;
		crypto_blake2b_update(context, (const uint8_t *)&before, sizeof(before));
		crypto_blake2b_update(context, (const uint8_t *)&time, sizeof(time));
	}
}

static void random_initialize(void)
{
	crypto_blake2b_ctx context;
	unsigned char seed[64];
	FILE *file;
	size_t count = 0;

	crypto_blake2b_init(&context, sizeof(random_state));
	file = fopen(SEED_PATH, "rb");
	if (file)
	{
		count = fread(seed, 1, sizeof(seed), file);
		fclose(file);
		crypto_blake2b_update(&context, seed, count);
	}
	random_gather(&context);
	crypto_blake2b_final(&context, random_state);
	/* the next start's seed, from this state (not the state itself) */
	crypto_blake2b(seed, sizeof(seed), random_state, sizeof(random_state));
	file = fopen(SEED_PATH, "wb");
	if (file)
	{
		fwrite(seed, 1, sizeof(seed), file);
		fclose(file);
	}
	random_ready = 1;
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	static int busy;
	unsigned char *cursor = buffer;
	unsigned long long counter = 0;

	while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
		OSYieldThread();
	if (!random_ready)
		random_initialize();
	while (size)
	{
		crypto_blake2b_ctx context;
		unsigned char block[64];
		OSTick tick = OSGetSystemTick();
		posix_ulong take = size < 32 ? size : 32;

		crypto_blake2b_init(&context, sizeof(block));
		crypto_blake2b_update(&context, random_state, sizeof(random_state));
		crypto_blake2b_update(&context, (const uint8_t *)&counter, sizeof(counter));
		crypto_blake2b_update(&context, (const uint8_t *)&tick, sizeof(tick));
		crypto_blake2b_final(&context, block);
		/* the first half out, the second the next state's part */
		memcpy(cursor, block, take);
		memcpy(random_state, block + 32, 32);
		crypto_wipe(block, sizeof(block));
		cursor += take;
		size -= take;
		counter++;
	}
	__atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/* (sys/random.h) */
ssize_t getrandom(void *buffer, size_t size, unsigned int flags)
{
	(void)flags;
	posix_random_bytes(buffer, (posix_ulong)size);
	return (ssize_t)size;
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	struct addrinfo hints, *results;
	posix_ulong address = 0;

	if (!network_ready())
		return 0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &results) != 0)
		return 0;
	if (results && results->ai_addr && results->ai_addr->sa_family == AF_INET)
		address = ((struct sockaddr_in *)results->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(results);
	return address;
}

/* ---------- the process */

int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	(void)index;
	(void)buffer;
	(void)size;
	return 0;
}

posix_ulong posix_process_id(void)
{
	return 1;
}

/* the user's secret: a key kept on the SD card beside the game */
int posix_user_secret(unsigned char *secret, int size)
{
	static const char path[] = "fs:/vol/external01/wiiu/apps/halo/user.key";
	FILE *file = fopen(path, "rb");
	int ok;

	if (file)
	{
		ok = (int)fread(secret, 1, (size_t)size, file) == size;
		fclose(file);
		if (ok)
			return 1;
	}
	file = fopen(path, "wb");
	if (!file)
		return 0;
	posix_random_bytes(secret, (posix_ulong)size);
	ok = (int)fwrite(secret, 1, (size_t)size, file) == size;
	fclose(file);
	if (!ok)
		remove(path);
	return ok;
}

/* (no desktop to register the invite links with, nor Discord) */
int posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

int posix_discord_connect(void)
{
	return -1;
}

int posix_discord_write(int handle, const void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

int posix_discord_read(int handle, void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

void posix_discord_close(int handle)
{
	(void)handle;
}

/* ---------- UPnP: not yet (internet play hosts without it) */

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	(void)external_address;
	(void)external_port;
	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "no UPnP on the Wii U yet");
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}

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

static int fail(void)
{
	switch (errno)
	{
	case EINTR: last_error = WSAEINTR; break;
	case EBADF: last_error = WSAEBADF; break;
	case EACCES: case EPERM: last_error = WSAEACCES; break;
	case EFAULT: last_error = WSAEFAULT; break;
	case EMFILE: case ENFILE: last_error = WSAEMFILE; break;
	case EAGAIN: last_error = WSAEWOULDBLOCK; break;
	case EINPROGRESS: last_error = WSAEINPROGRESS; break;
	case EALREADY: last_error = WSAEALREADY; break;
	case ENOTSOCK: last_error = WSAENOTSOCK; break;
	case EDESTADDRREQ: last_error = WSAEDESTADDRREQ; break;
	case EMSGSIZE: last_error = WSAEMSGSIZE; break;
	case EPROTOTYPE: last_error = WSAEPROTOTYPE; break;
	case ENOPROTOOPT: last_error = WSAENOPROTOOPT; break;
	case EPROTONOSUPPORT: last_error = WSAEPROTONOSUPPORT; break;
	case EOPNOTSUPP: last_error = WSAEOPNOTSUPP; break;
	case EAFNOSUPPORT: last_error = WSAEAFNOSUPPORT; break;
	case EADDRINUSE: last_error = WSAEADDRINUSE; break;
	case EADDRNOTAVAIL: last_error = WSAEADDRNOTAVAIL; break;
	case ENETDOWN: last_error = WSAENETDOWN; break;
	case ENETUNREACH: last_error = WSAENETUNREACH; break;
	case ENETRESET: last_error = WSAENETRESET; break;
	case ECONNABORTED: last_error = WSAECONNABORTED; break;
	case ECONNRESET: case EPIPE: last_error = WSAECONNRESET; break;
	case ENOBUFS: case ENOMEM: last_error = WSAENOBUFS; break;
	case EISCONN: last_error = WSAEISCONN; break;
	case ENOTCONN: last_error = WSAENOTCONN; break;
	case ETIMEDOUT: last_error = WSAETIMEDOUT; break;
	case ECONNREFUSED: last_error = WSAECONNREFUSED; break;
	case EHOSTUNREACH: last_error = WSAEHOSTUNREACH; break;
	default: last_error = WSAEINVAL; break;
	}
	return -1;
}

static int succeed(int result)
{
	if (result < 0)
		return fail();
	last_error = 0;
	return result;
}

int posix_socket_last_error(void)
{
	return last_error;
}

int posix_socket(int family, int type, int protocol)
{
	return succeed(socket(family, type, protocol));
}

int posix_socket_close(int socket)
{
	return succeed(close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	return succeed(bind(socket, address, (socklen_t)address_length));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	int result = connect(socket, address, (socklen_t)address_length);

	if (result < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK))
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	return succeed(result);
}

int posix_socket_listen(int socket, int backlog)
{
	return succeed(listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	socklen_t length = address_length ? (socklen_t)*address_length : 0;
	int result = accept(socket, address, address_length ? &length : NULL);

	if (address_length)
		*address_length = (int)length;
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return succeed((int)send(socket, buffer, (size_t)length, flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	int result = (int)sendto(socket, buffer, (size_t)length, flags, address, (socklen_t)address_length);

	if (result < 0 && errno == EISCONN)
		result = (int)send(socket, buffer, (size_t)length, flags);
	return succeed(result);
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed((int)recv(socket, buffer, (size_t)length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	socklen_t socket_length = address && address_length ? (socklen_t)*address_length : 0;
	int result = (int)recvfrom(socket, buffer, (size_t)length, flags, address && address_length ? address : NULL,
		address && address_length ? &socket_length : NULL);

	if (address_length)
		*address_length = (int)socket_length;
	return succeed(result);
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed(shutdown(socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int value = nonblocking ? 1 : 0;

	return succeed(setsockopt(socket, SOL_SOCKET, SO_NONBLOCK, &value, sizeof(value)));
}

int posix_socket_set_nodelay(int socket)
{
	int value = 1;

	return succeed(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result = ioctl(socket, FIONREAD, &available);

	if (result >= 0)
		*count = (posix_ulong)available;
	return succeed(result);
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

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	int host_level, host_name;

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		/* Xbox-only options such as SO_ENCRYPT have nothing to do here */
		last_error = 0;
		return 0;
	}
	return succeed(setsockopt(socket, host_level, host_name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	int host_level, host_name;
	socklen_t socket_length = (socklen_t)*length;
	int result;

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	result = getsockopt(socket, host_level, host_name, value, &socket_length);
	*length = (int)socket_length;
	return succeed(result);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	socklen_t length = address_length ? (socklen_t)*address_length : 0;
	int result = getsockname(socket, address, address_length ? &length : NULL);

	if (address_length)
		*address_length = (int)length;
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	socklen_t length = address_length ? (socklen_t)*address_length : 0;
	int result = getpeername(socket, address, address_length ? &length : NULL);

	if (address_length)
		*address_length = (int)length;
	return succeed(result);
}

/* Winsock's select over lists of sockets, by poll: read for data, the end
or an error, write for room or an error, error for a connect that failed */
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
		return fail();
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
		free(descriptors);
		return fail();
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
				fail();
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

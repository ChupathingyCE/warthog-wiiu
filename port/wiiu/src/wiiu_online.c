/*
WIIU_ONLINE.C

The console game list (wiiu_game_list.h) and, before its first fetch, a
network self-test at start-up, both on a thread of the platform layer's
(thread_stop.h: it stops at the game's end), so nothing waits on them:

1. The network up (wiiu_network_start, wiiu_net.c: Butter and Jelly's
   order), with the console's address's kind.
2. The byte order: the game's messages turned to the PC's and back,
   against the PC's bytes (network_wire_self_test, the game's
   port/linux/game/network_distributed.c).
3. UDP: a datagram to this console's own address and port, read back (the
   loopback's if that does not come back).
4. HTTPS: GET /v1/console/games from stats.chupathingy.org (wiiu_https.c,
   the site's key pinned), parsed; the list is the Multiplayer menu's
   ONLINE GAMES (game/wiiu_browser_screen.c). When that fails, DNS and HTTP:
   warthog.milenko.org looked up, and the same GET over plain HTTP/1.0 as
   the original Xbox and the Xbox 360 ask for it. A key or a name refused
   is refused again: plain HTTP for the rest of the run.

Each step is one line in port.txt ("net test: ..."), and each list says
which way it came. Never an address: only its kind. The sockets are
wiiu_net.c's (posix.h), so the list and the self-test go through what the
game's own networking uses.

After the list, the same thread fetches the signed legacy table when it
is due (wiiu_https_table_refresh), so its stack has room for HTTPS and the
table's document.

The Xbox 360's (ChupathingyCE/warthog-360, port/xbox360/src/x360_online.c)
on the Wii U's threads and clock.
*/

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <coreinit/time.h>
#include <nsysnet/_socket.h>

#include "posix.h"
#include "thread_stop.h"
#include "wiiu_game_list.h"
#include "wiiu_port.h"
#include "port/console/https/console_https.h"
#include "port/linux/include/delta.h"

#define FETCH_TIMEOUT 15000
#define ATTEMPTS 3
#define RETRY_WAIT 10000
#define ECHO_WAIT 2000
/* the list's thread's stack: wut's own for a thread given none (128 KB,
wut_thread.c), CONSOLE_HTTPS_STACK_SIZE more for the TLS session's arena
and Mbed TLS's frames, and DELTA_REFRESH_STACK_SIZE for the signed table's
document; from the system's heap, given back when the thread ends */
#define ONLINE_THREAD_STACK (128 * 1024 + CONSOLE_HTTPS_STACK_SIZE + DELTA_REFRESH_STACK_SIZE)

/* Winsock's codes, as posix_socket_last_error gives them (wiiu_net.c) */
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036

/* (the game's: port/linux/game/network_distributed.c, a big-endian build's) */
long network_wire_self_test(char *report, long report_size);

enum
{
	_list_idle,
	_list_fetching,
	_list_done,
	_list_failed
};

/* the fetch's thread writes fetched, then the state (a release store, so
the list is whole before it); the main thread takes it into shown once the
state is done (an acquire load), and only then may a new fetch start */
static int list_state = _list_idle;
static int list_taken = 1;
static struct game_list fetched;
static int list_error;
/* the fetch's response (one fetch at a time) */
static char response_buffer[GAME_LIST_RESPONSE_SIZE];
/* the main thread's: the last list that came, kept while a refresh fails */
static struct game_list shown;
static int shown_lists;
static int shown_error;
static int shown_state = WIIU_GAME_LIST_NONE;
/* the self-test runs once, before the first fetch */
static int self_tested;
/* the list came over HTTPS (the fetch's thread's); HTTPS was refused for
good (a key or a name) */
static int list_secure;
static int https_refused;

static void set_state(int state)
{
	__atomic_store_n(&list_state, state, __ATOMIC_RELEASE);
}

static int get_state(void)
{
	return __atomic_load_n(&list_state, __ATOMIC_ACQUIRE);
}

static unsigned long now_ms(void)
{
	return (unsigned long)OSTicksToMilliseconds(OSGetSystemTime());
}

/* ---------- waits */

/* until the socket can be read (or written) or the deadline: 1 ready (a
quarter of a second at a time, so the game's end is seen) */
static int wait_ready(int handle, int write, unsigned long deadline)
{
	for (;;)
	{
		int handles[1], count = 1;
		long left = (long)(deadline - now_ms());

		if (left <= 0 || thread_stop_requested())
			return 0;
		if (left > 250)
			left = 250;
		handles[0] = handle;
		if (posix_socket_select(write ? NULL : handles, write ? NULL : &count, write ? handles : NULL,
			write ? &count : NULL, NULL, NULL, left / 1000, (left % 1000) * 1000, 0) > 0 && count > 0)
		{
			return 1;
		}
	}
}

/* ---------- the HTTP request */

/* whether the response's first size bytes hold its headers' end, and the
response's whole size if they give a Content-Length (else 0) */
static int header_end(const char *data, unsigned long size, unsigned long *total)
{
	struct game_list_response response;
	unsigned long index;

	for (index = 1; index < size; index++)
	{
		if (data[index] == '\n' && (data[index - 1] == '\n' || (index >= 2 && data[index - 1] == '\r' &&
			data[index - 2] == '\n')))
		{
			int result = game_list_parse_response(data, size, &response);

			*total = 0;
			if ((result == GAME_LIST_OK || result == GAME_LIST_ERROR_BODY_TRUNCATED) && response.content_length >= 0)
				*total = index + 1 + (unsigned long)response.content_length;
			return 1;
		}
	}
	return 0;
}

/* GET path from address:port (network order), with Host: host, into
buffer (the whole response, never past size), within timeout_ms */
static int http_get(unsigned long address, unsigned short port, const char *host, const char *path, char *buffer,
	unsigned long size, unsigned long *received, unsigned long timeout_ms)
{
	char request[512];
	struct sockaddr_in target;
	unsigned long deadline = now_ms() + timeout_ms;
	unsigned long sent = 0, total = 0;
	int headers_done = 0, length, handle, result = GAME_LIST_OK;

	*received = 0;
	length = snprintf(request, sizeof(request) - 1,
		"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: WarthogWiiU/1\r\nAccept: text/plain\r\n"
		"Accept-Encoding: identity\r\nConnection: close\r\n\r\n", path, host);
	if (length <= 0 || length >= (int)sizeof(request) - 1)
		return GAME_LIST_ERROR_SEND;
	handle = posix_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (handle < 0)
		return GAME_LIST_ERROR_CONNECT;
	memset(&target, 0, sizeof(target));
	target.sin_family = AF_INET;
	target.sin_port = htons(port);
	target.sin_addr.s_addr = (in_addr_t)address;
	/* (not blocking, so the deadline bounds the connect too) */
	posix_socket_set_nonblocking(handle, 1);
	if (posix_socket_connect(handle, &target, sizeof(target)) != 0)
	{
		int code = posix_socket_last_error();

		if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS)
		{
			posix_socket_close(handle);
			return GAME_LIST_ERROR_CONNECT;
		}
		if (!wait_ready(handle, 1, deadline))
		{
			posix_socket_close(handle);
			return GAME_LIST_ERROR_CONNECT;
		}
	}
	while (sent < (unsigned long)length)
	{
		int count;

		if (!wait_ready(handle, 1, deadline))
		{
			result = GAME_LIST_ERROR_TIMEOUT;
			break;
		}
		count = posix_socket_send(handle, request + sent, length - (int)sent, 0);
		if (count <= 0)
		{
			if (count < 0 && posix_socket_last_error() == WSAEWOULDBLOCK)
				continue;
			result = GAME_LIST_ERROR_SEND;
			break;
		}
		sent += (unsigned long)count;
	}
	/* to the server's close, or the Content-Length's end, never past size */
	while (result == GAME_LIST_OK)
	{
		int count;

		if (*received >= size)
		{
			result = GAME_LIST_ERROR_TOO_LARGE;
			break;
		}
		if (!wait_ready(handle, 0, deadline))
		{
			result = GAME_LIST_ERROR_TIMEOUT;
			break;
		}
		count = posix_socket_recv(handle, buffer + *received, (int)(size - *received), 0);
		if (count < 0)
		{
			if (posix_socket_last_error() == WSAEWOULDBLOCK)
				continue;
			result = GAME_LIST_ERROR_RECEIVE;
			break;
		}
		if (count == 0)
			break;
		*received += (unsigned long)count;
		if (!headers_done)
			headers_done = header_end(buffer, *received, &total);
		if (headers_done && total && *received >= total)
			break;
	}
	posix_socket_close(handle);
	return result;
}

/* ---------- the self-test's UDP step */

/* a datagram to this console's own port at address (network order), read
back: 1 if it came back whole, its round trip in *milliseconds */
static int udp_echo_to(unsigned long address, unsigned long *milliseconds, const char **failure)
{
	static const char probe[] = "warthog wiiu echo";
	struct sockaddr_in local, to, from;
	int length = sizeof(local), from_length = sizeof(from), handle, count;
	char reply[64];
	unsigned long start;

	*failure = "no socket";
	handle = posix_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (handle < 0)
		return 0;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	*failure = "could not bind";
	if (posix_socket_bind(handle, &local, sizeof(local)) != 0 ||
		posix_socket_getsockname(handle, &local, &length) != 0 || !local.sin_port)
	{
		posix_socket_close(handle);
		return 0;
	}
	posix_socket_set_nonblocking(handle, 1);
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = local.sin_port;
	to.sin_addr.s_addr = (in_addr_t)address;
	start = now_ms();
	*failure = "send failed";
	if (posix_socket_sendto(handle, probe, sizeof(probe), 0, &to, sizeof(to)) != (int)sizeof(probe))
	{
		posix_socket_close(handle);
		return 0;
	}
	*failure = "nothing came back";
	if (!wait_ready(handle, 0, start + ECHO_WAIT))
	{
		posix_socket_close(handle);
		return 0;
	}
	count = posix_socket_recvfrom(handle, reply, sizeof(reply), 0, &from, &from_length);
	*milliseconds = now_ms() - start;
	posix_socket_close(handle);
	*failure = "a different datagram came back";
	return count == (int)sizeof(probe) && !memcmp(reply, probe, sizeof(probe));
}

/* ---------- a datagram socket's longest receive

The console refused internet play's tunnel's 2048-byte receives with
EMSGSIZE whatever the datagram's size (wiiu_net.c, "a datagram socket's
receive length"). A datagram to the loopback, then nsysnet's own recvfrom
asked with shorter and shorter lengths: the lengths refused, the first
taken, and whether the datagram was still there after a refusal. */

static void receive_length_test(void)
{
	static const int lengths[] = { 4096, 2048, 1500, 1472, 1400, 1280, 1200, 512 };
	static char data[4096];
	static const char probe[] = "warthog wiiu length";
	struct sockaddr_in local, from;
	int length = sizeof(local), handle, index;
	char refused[96] = "";
	size_t used = 0;

	handle = posix_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (handle < 0)
		return;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(0x7F000001UL);
	if (posix_socket_bind(handle, &local, sizeof(local)) != 0 ||
		posix_socket_getsockname(handle, &local, &length) != 0 || posix_socket_set_nonblocking(handle, 1) != 0 ||
		posix_socket_sendto(handle, probe, sizeof(probe), 0, &local, sizeof(local)) != (int)sizeof(probe) ||
		!wait_ready(handle, 0, now_ms() + ECHO_WAIT))
	{
		wiiu_log("net test: receive lengths: the loopback datagram did not come; not tested");
		posix_socket_close(handle);
		return;
	}
	for (index = 0; index < (int)(sizeof(lengths) / sizeof(*lengths)); index++)
	{
		socklen_t from_length = sizeof(from);
		int result = (int)recvfrom(handle, data, (size_t)lengths[index], 0, (struct sockaddr *)&from, &from_length);
		int error = errno;

		if (result >= 0)
		{
			wiiu_log("net test: receive lengths: %d taken (%d bytes came)%s%s%s", lengths[index], result,
				used ? "; refused: " : "", refused, used ? " (the datagram stayed for the next)" : "");
			posix_socket_close(handle);
			return;
		}
		if (error == EAGAIN)
		{
			wiiu_log("net test: receive lengths: %d taken but the datagram was gone%s%s", lengths[index],
				used ? "; refused: " : "", refused);
			posix_socket_close(handle);
			return;
		}
		if (used < sizeof(refused) - 24)
		{
			used += (size_t)snprintf(refused + used, sizeof(refused) - used, "%s%d (errno %d, nsysnet %d)",
				used ? ", " : "", lengths[index], error, RPLWRAP(socketlasterr)());
		}
	}
	wiiu_log("net test: receive lengths: none taken; refused: %s", refused);
	posix_socket_close(handle);
}

/* ---------- UDP out to the internet: a STUN binding request (RFC 5389)

Internet play's tunnel is a UDP socket that reaches public addresses: STUN
servers (this machine's public address), the host. The echo above never
leaves the console. Three sockets ask stun.cloudflare.com, each the way the
tunnel could have made it, so a run says which way works:

1. a plain socket, as the echo's (the system's buffer sizes);
2. the same, made on this thread and used from another's, as the tunnel's
   (made where WSAStartup is called, p2p_initialize, used from internet
   play's thread);
3. a socket given 1 MB buffers through nsysnet itself (wiiu_net.c no longer
   passes them on), as the tunnel and the game asked before this build.

Each line gives the setsockopt results (errno and nsysnet's code), and
whether the answer came. */

#define STUN_HOST "stun.cloudflare.com"
#define STUN_PORT 3478
#define STUN_WAIT 3000

struct stun_ask
{
	int handle;
	unsigned long server;
	int answered;
	unsigned long milliseconds;
	const char *failure;
	int send_error;
	int done;
};

static void stun_ask_run(struct stun_ask *ask)
{
	unsigned char request[20], reply[512];
	struct sockaddr_in to, from;
	int from_length = sizeof(from), count;
	unsigned long start;

	memset(request, 0, sizeof(request));
	request[1] = 0x01;
	request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
	posix_random_bytes(request + 8, 12);
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(STUN_PORT);
	to.sin_addr.s_addr = (in_addr_t)ask->server;
	start = now_ms();
	if (posix_socket_sendto(ask->handle, request, sizeof(request), 0, &to, sizeof(to)) != (int)sizeof(request))
	{
		ask->send_error = posix_socket_last_error();
		ask->failure = "the send failed";
		return;
	}
	for (;;)
	{
		if (!wait_ready(ask->handle, 0, start + STUN_WAIT))
		{
			ask->failure = "no answer within 3 s";
			return;
		}
		from_length = sizeof(from);
		count = posix_socket_recvfrom(ask->handle, reply, sizeof(reply), 0, &from, &from_length);
		if (count >= 20 && reply[0] == 0x01 && reply[1] == 0x01 && !memcmp(reply + 8, request + 8, 12))
			break;
		if (count < 0 && posix_socket_last_error() != WSAEWOULDBLOCK)
		{
			ask->failure = "the receive failed";
			return;
		}
	}
	ask->milliseconds = now_ms() - start;
	ask->answered = 1;
}

static void *stun_ask_thread(void *parameter)
{
	struct stun_ask *ask = parameter;

	stun_ask_run(ask);
	__atomic_store_n(&ask->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

/* a UDP socket bound to any address, port 0, not blocking; -1 */
static int stun_socket(void)
{
	struct sockaddr_in local;
	int handle = posix_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (handle < 0)
		return -1;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	if (posix_socket_bind(handle, &local, sizeof(local)) != 0 || posix_socket_set_nonblocking(handle, 1) != 0)
	{
		posix_socket_close(handle);
		return -1;
	}
	return handle;
}

/* nsysnet's own setsockopt of a buffer size, and what the socket has after */
static void stun_raw_buffer(int handle, int option, const char *name, char *text, size_t size)
{
	int value = 1 << 20, result, error, nsysnet, after = -1;
	socklen_t length = sizeof(after);

	errno = 0;
	result = setsockopt(handle, SOL_SOCKET, option, &value, sizeof(value));
	error = errno;
	nsysnet = RPLWRAP(socketlasterr)();
	getsockopt(handle, SOL_SOCKET, option, &after, &length);
	if (result == 0)
		snprintf(text, size, "%s 1048576 taken (it reads %d)", name, after);
	else
		snprintf(text, size, "%s 1048576 refused (errno %d, nsysnet %d; it reads %d)", name, error, nsysnet, after);
}

static void stun_tests(void)
{
	static const char *const ways[3] =
	{
		"a plain socket",
		"a socket made on this thread, used from another",
		"a socket given 1 MB buffers",
	};
	unsigned long server = (unsigned long)posix_resolve_ipv4(STUN_HOST);
	int way;

	if (!server)
	{
		wiiu_log("net test: UDP out to the internet: DNS %s FAILED; not tested", STUN_HOST);
		return;
	}
	for (way = 0; way < 3; way++)
	{
		/* (kept: a thread that does not finish keeps its socket and this) */
		static struct stun_ask asks[3];
		struct stun_ask *ask = &asks[way];
		char buffers[2][96] = { "", "" };

		memset(ask, 0, sizeof(*ask));
		ask->server = server;
		ask->handle = stun_socket();
		if (ask->handle < 0)
		{
			wiiu_log("net test: UDP out to the internet (STUN, %s) FAILED: no socket (Winsock %d)", ways[way],
				posix_socket_last_error());
			continue;
		}
		if (way == 2)
		{
			stun_raw_buffer(ask->handle, SO_SNDBUF, "SO_SNDBUF", buffers[0], sizeof(buffers[0]));
			stun_raw_buffer(ask->handle, SO_RCVBUF, "SO_RCVBUF", buffers[1], sizeof(buffers[1]));
		}
		if (way == 1)
		{
			unsigned long deadline = now_ms() + STUN_WAIT + 2000;

			if (thread_stop_create("net test STUN", stun_ask_thread, ask) != 0)
			{
				ask->failure = "no thread";
			}
			else
			{
				while (!__atomic_load_n(&ask->done, __ATOMIC_ACQUIRE) && (long)(deadline - now_ms()) > 0)
				{
					if (thread_stop_sleep(50))
						break;
				}
				if (!__atomic_load_n(&ask->done, __ATOMIC_ACQUIRE))
				{
					/* (the thread still has the socket: left open) */
					wiiu_log("net test: UDP out to the internet (STUN, %s): the thread did not finish", ways[way]);
					continue;
				}
			}
		}
		else
		{
			stun_ask_run(ask);
		}
		posix_socket_close(ask->handle);
		if (ask->answered)
		{
			wiiu_log("net test: UDP out to the internet (STUN %s, %s): answered (%lu ms)%s%s%s%s", STUN_HOST,
				ways[way], ask->milliseconds, *buffers[0] ? "; " : "", buffers[0], *buffers[1] ? "; " : "", buffers[1]);
		}
		else
		{
			wiiu_log("net test: UDP out to the internet (STUN %s, %s) FAILED: %s%s%s%s%s", STUN_HOST, ways[way],
				ask->failure ? ask->failure : "?", *buffers[0] ? "; " : "", buffers[0], *buffers[1] ? "; " : "",
				buffers[1]);
			if (ask->send_error)
				wiiu_log("net test:   the send's Winsock error %d (errno: the \"net: sendto\" line above)",
					ask->send_error);
		}
	}
}

/* the console's own address (the way a datagram goes out to the network
and back), then the loopback's: which came back, NULL for neither */
static const char *udp_echo(unsigned long *milliseconds, const char **failure)
{
	unsigned long own = (unsigned long)posix_local_ipv4_address();

	if (own && udp_echo_to(own, milliseconds, failure))
		return "its own address";
	if (udp_echo_to(htonl(0x7F000001UL), milliseconds, failure))
		return own ? "the loopback (not its own address)" : "the loopback (it has no address)";
	return NULL;
}

/* ---------- the list */

/* the list over HTTPS from the site (its key pinned: the game's
docs/console-https.md): GAME_LIST_OK and the list, or another result and
the plain HTTP list is asked instead */
static int fetch_https(char *buffer, int test)
{
	struct game_list_response response;
	unsigned long received = 0, start = now_ms();
	int result;

	result = console_https_request(CONSOLE_HTTPS_SITE, GAME_LIST_PATH, NULL, NULL, buffer, GAME_LIST_RESPONSE_SIZE,
		&received, FETCH_TIMEOUT);
	if (result != CONSOLE_HTTPS_OK)
	{
		if (test)
			wiiu_log("net test: HTTPS GET %s from %s FAILED: %s (%lu ms)", GAME_LIST_PATH, CONSOLE_HTTPS_SITE,
				console_https_error_string(result), now_ms() - start);
		else
			wiiu_log("game list: HTTPS: %s; asking over plain HTTP", console_https_error_string(result));
		/* (a key or a name refused is refused again: plain HTTP for the rest
		of this run) */
		if (result == CONSOLE_HTTPS_ERROR_KEY || result == CONSOLE_HTTPS_ERROR_NAME)
		{
			https_refused = 1;
			wiiu_log("game list: HTTPS refused (%s): plain HTTP for the rest of this run",
				console_https_error_string(result));
		}
		return GAME_LIST_ERROR_CONNECT;
	}
	result = game_list_parse_response(buffer, received, &response);
	if (result == GAME_LIST_OK && response.status != 200)
		result = GAME_LIST_ERROR_HTTP;
	if (result == GAME_LIST_OK)
		result = game_list_parse(response.body, response.body_size, &fetched);
	if (test)
	{
		if (result == GAME_LIST_OK)
			wiiu_log("net test: HTTPS GET %s from %s: status 200, %lu bytes, %d games (%lu ms)", GAME_LIST_PATH,
				CONSOLE_HTTPS_SITE, received, fetched.count, now_ms() - start);
		else
			wiiu_log("net test: HTTPS GET %s from %s FAILED: %s, %lu bytes received (%lu ms)", GAME_LIST_PATH,
				CONSOLE_HTTPS_SITE, game_list_error_string(result), received, now_ms() - start);
	}
	else if (result != GAME_LIST_OK && result != GAME_LIST_ERROR_FORMAT)
	{
		wiiu_log("game list: HTTPS: %s; asking over plain HTTP", game_list_error_string(result));
	}
	if (result == GAME_LIST_OK)
		list_secure = 1;
	return result;
}

static int fetch_once(char *buffer, int test, int attempt)
{
	struct game_list_response response;
	unsigned long address, received = 0;
	unsigned long start;
	int result;

	list_secure = 0;
	/* (HTTPS first, on each fetch's first try; a list that came over it
	ends the fetch, anything else falls back) */
	if (attempt == 1 && !https_refused && console_https_available())
	{
		result = fetch_https(buffer, test);
		if (result == GAME_LIST_OK || result == GAME_LIST_ERROR_FORMAT)
			return result;
	}
	start = now_ms();
	address = (unsigned long)posix_resolve_ipv4(GAME_LIST_HOST);
	if (test)
	{
		if (address)
			wiiu_log("net test: DNS %s resolved (%lu ms)", GAME_LIST_HOST, now_ms() - start);
		else
			wiiu_log("net test: DNS %s FAILED (%lu ms)", GAME_LIST_HOST, now_ms() - start);
	}
	if (!address)
		return GAME_LIST_ERROR_RESOLVE;
	start = now_ms();
	result = http_get(address, GAME_LIST_PORT, GAME_LIST_HOST, GAME_LIST_PATH, buffer, GAME_LIST_RESPONSE_SIZE,
		&received, FETCH_TIMEOUT);
	if (result == GAME_LIST_OK)
		result = game_list_parse_response(buffer, received, &response);
	if (result == GAME_LIST_OK && response.status != 200)
		result = GAME_LIST_ERROR_HTTP;
	if (result == GAME_LIST_OK)
		result = game_list_parse(response.body, response.body_size, &fetched);
	if (test)
	{
		if (result == GAME_LIST_OK)
		{
			int index, warthog = 0;

			for (index = 0; index < fetched.count; index++)
				warthog += strstr(fetched.games[index].name, "Warthog") != NULL;
			wiiu_log("net test: HTTP GET %s: status 200, %lu bytes, %d games (%d named Warthog) (%lu ms)",
				GAME_LIST_PATH, received, fetched.count, warthog, now_ms() - start);
		}
		else
		{
			wiiu_log("net test: HTTP GET %s FAILED: %s, %lu bytes received (%lu ms)", GAME_LIST_PATH,
				game_list_error_string(result), received, now_ms() - start);
		}
	}
	return result;
}

static void self_test(void)
{
	unsigned long milliseconds = 0;
	const char *failure, *way;
	char report[256];
	long failed;

	wiiu_log("net test: start");
	failed = network_wire_self_test(report, (long)sizeof(report));
	wiiu_log("net test: byte order %s: %s", failed ? "FAILED" : "ok", report);
	if (!wiiu_network_start())
	{
		wiiu_log("net test: the network did not start; no UDP, DNS or HTTP test");
		return;
	}
	if ((way = udp_echo(&milliseconds, &failure)) != NULL)
		wiiu_log("net test: UDP to this console's own port came back, by %s (%lu ms)", way, milliseconds);
	else
		wiiu_log("net test: UDP to this console's own port FAILED: %s", failure);
	/* (the loopback on its own too: internet play hands the host's
	datagrams to the game by it, network.address being empty) */
	if (udp_echo_to(htonl(0x7F000001UL), &milliseconds, &failure))
		wiiu_log("net test: UDP to the loopback came back (%lu ms)", milliseconds);
	else
		wiiu_log("net test: UDP to the loopback FAILED: %s", failure);
	receive_length_test();
	stun_tests();
}

static void online_main(void)
{
	int attempt, result = GAME_LIST_ERROR_CONNECT, test = !self_tested;

	if (test)
	{
		self_tested = 1;
		self_test();
	}
	if (!wiiu_network_start())
	{
		list_error = GAME_LIST_ERROR_CONNECT;
		set_state(_list_failed);
		return;
	}
	wiiu_https_start();
	for (attempt = 1; attempt <= ATTEMPTS && !thread_stop_requested(); attempt++)
	{
		result = fetch_once(response_buffer, test && attempt == 1, attempt);
		if (result == GAME_LIST_OK || result == GAME_LIST_ERROR_FORMAT)
			break;
		wiiu_log("game list: try %d of %d: %s", attempt, ATTEMPTS, game_list_error_string(result));
		if (attempt < ATTEMPTS && thread_stop_sleep(RETRY_WAIT))
			break;
	}
	list_error = result;
	if (result == GAME_LIST_OK)
		wiiu_log("game list: %d games (format %d) from %s%s", fetched.count, fetched.format,
			list_secure ? CONSOLE_HTTPS_SITE " over HTTPS" : GAME_LIST_HOST " over plain HTTP",
			fetched.skipped ? ", some lines left out" : "");
	if (test)
		wiiu_log("net test: done");
	set_state(result == GAME_LIST_OK ? _list_done : _list_failed);
	/* (the signed legacy table, when it is due: after the list, so that the
	list is never held up by it) */
	wiiu_https_table_refresh();
}

static void *online_thread(void *parameter)
{
	online_main();
	/* (counted for the game's end since it was made: online_thread_start) */
	thread_stop_leave((int)(long)parameter);
	return NULL;
}

/* the list's thread: as thread_stop_create makes one (counted, so the
game's end waits for it), with a stack of ONLINE_THREAD_STACK; 0 when it
started */
static int online_thread_start(void)
{
	pthread_attr_t attributes;
	pthread_t thread;
	int slot = thread_stop_enter("game list", 1), result;

	/* (a full table still starts the thread, uncounted; only the end stops
	new ones) */
	if (slot < 0 && thread_stop_requested())
		return -1;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, ONLINE_THREAD_STACK);
	result = pthread_create(&thread, &attributes, online_thread, (void *)(long)slot);
	pthread_attr_destroy(&attributes);
	if (result != 0)
	{
		wiiu_log("game list: no thread (%d KB of stack asked for)", ONLINE_THREAD_STACK / 1024);
		thread_stop_leave(slot);
		return -1;
	}
	pthread_detach(thread);
	return 0;
}

/* the start-up's self-test and first list (wiiu_proc.c) */
void wiiu_online_start(void)
{
	wiiu_game_list_request();
}

void wiiu_game_list_request(void)
{
	/* (not while a fetch runs, or before its list is taken) */
	if (get_state() == _list_fetching || !list_taken)
		return;
	list_taken = 0;
	shown_state = WIIU_GAME_LIST_FETCHING;
	set_state(_list_fetching);
	if (online_thread_start() != 0)
	{
		list_error = GAME_LIST_ERROR_CONNECT;
		set_state(_list_failed);
	}
}

/* a list that came taken (the main thread's) */
static void take(void)
{
	int state = get_state();

	if (list_taken || (state != _list_done && state != _list_failed))
		return;
	list_taken = 1;
	if (state == _list_failed)
	{
		shown_error = list_error;
		shown_state = WIIU_GAME_LIST_FAILED;
		wiiu_log("game list: could not get the list from %s or %s (%s)", CONSOLE_HTTPS_SITE, GAME_LIST_HOST,
			game_list_error_string(list_error));
		return;
	}
	memcpy(&shown, &fetched, sizeof(shown));
	shown_error = GAME_LIST_OK;
	shown_state = WIIU_GAME_LIST_READY;
	shown_lists++;
}

const struct game_list *wiiu_game_list_get(int *lists)
{
	take();
	if (lists)
		*lists = shown_lists;
	return shown_lists ? &shown : NULL;
}

int wiiu_game_list_state(void)
{
	take();
	return shown_state;
}

const char *wiiu_game_list_error(void)
{
	return game_list_error_string(shown_error);
}

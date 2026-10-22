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
4. DNS: warthog.milenko.org looked up.
5. HTTP: GET /v1/console/games from warthog.milenko.org, plain HTTP/1.0 as
   the original Xbox and the Xbox 360 ask for it, parsed; the list is the
   Multiplayer menu's ONLINE GAMES (game/wiiu_browser_screen.c).

Each step is one line in port.txt ("net test: ..."). Never an address:
only its kind. The sockets are wiiu_net.c's (posix.h), so the list and the
self-test go through what the game's own networking uses.

The Xbox 360's (ChupathingyCE/warthog-360, port/xbox360/src/x360_online.c)
on the Wii U's threads and clock.
*/

#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <coreinit/time.h>

#include "posix.h"
#include "thread_stop.h"
#include "wiiu_game_list.h"
#include "wiiu_port.h"

#define FETCH_TIMEOUT 15000
#define ATTEMPTS 3
#define RETRY_WAIT 10000
#define ECHO_WAIT 2000

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

static int fetch_once(char *buffer, int test)
{
	struct game_list_response response;
	unsigned long address, received = 0;
	unsigned long start = now_ms();
	int result;

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
}

static void *online_thread(void *parameter)
{
	int attempt, result = GAME_LIST_ERROR_CONNECT, test = !self_tested;

	(void)parameter;
	if (test)
	{
		self_tested = 1;
		self_test();
	}
	if (!wiiu_network_start())
	{
		list_error = GAME_LIST_ERROR_CONNECT;
		set_state(_list_failed);
		return NULL;
	}
	for (attempt = 1; attempt <= ATTEMPTS && !thread_stop_requested(); attempt++)
	{
		result = fetch_once(response_buffer, test && attempt == 1);
		if (result == GAME_LIST_OK || result == GAME_LIST_ERROR_FORMAT)
			break;
		wiiu_log("game list: try %d of %d: %s", attempt, ATTEMPTS, game_list_error_string(result));
		if (attempt < ATTEMPTS && thread_stop_sleep(RETRY_WAIT))
			break;
	}
	list_error = result;
	if (result == GAME_LIST_OK)
		wiiu_log("game list: %d games (format %d)%s", fetched.count, fetched.format,
			fetched.skipped ? ", some lines left out" : "");
	if (test)
		wiiu_log("net test: done");
	set_state(result == GAME_LIST_OK ? _list_done : _list_failed);
	return NULL;
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
	if (thread_stop_create("game list", online_thread, NULL) != 0)
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
		wiiu_log("game list: could not get the list from %s (%s)", GAME_LIST_HOST,
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

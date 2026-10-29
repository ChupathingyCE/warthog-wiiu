/*
WIIU_HTTPS.C

HTTPS on the Wii U (wiiu_port.h): the game's shared client
(chupathingyce/port/console/https, Mbed TLS, the game's
docs/console-https.md) over nsysnet's BSD sockets, through wiiu_net.c's
posix_socket_* as the plain list's request (wiiu_online.c), the hosts
looked up as the plain list looks them up (posix_resolve_ipv4), and an
OSMutex so that one request runs at a time. The game list
(wiiu_online.c) and the signed legacy table (the game's delta.c, with
HALO_CONSOLE_HTTPS) call through here, on the list's thread.

Random numbers: wiiu_net.c's posix_random_bytes, its BLAKE2b pool (seeded
from the last start's seed file and the system tick's jitter, mixed again
at each read). wut has no declaration of a system random source: its
nlibnss import (NSSGetRandom) comes with no header, so its arguments would
be a guess.

Every wait gives up at the game's end (thread_stop_requested), a quarter
of a second at a time, so a request never holds the end up.

The log names the host and what went wrong, never an address.
*/

#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <coreinit/mutex.h>
#include <coreinit/time.h>

#include "posix.h"
#include "thread_stop.h"
#include "wiiu_port.h"
#include "port/console/https/console_https.h"

/* the game's (port/linux/src/delta.c, HALO_CONSOLE_HTTPS) */
void delta_legacy_start(void);
int delta_legacy_refresh(void);

/* (port/linux/src/port_config.h) */
void config_folder(char *path, size_t size);
/* the newest pin document's file (console_https.h's pins_path) */
static char pins_path[300];

/* Winsock's codes, as posix_socket_last_error gives them (wiiu_net.c) */
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAECONNRESET 10054

/* the signed table again this often, and this long after a failure, as the
desktop builds (delta.c's DELTA_REFRESH_INTERVAL and DELTA_RETRY_INTERVAL) */
#define TABLE_REFRESH (4ul * 60 * 60 * 1000)
#define TABLE_RETRY (30ul * 60 * 1000)
/* how long the list's thread waits for the game's start to have read the
cached table (wiiu_https_table_start) before it fetches one */
#define TABLE_START_WAIT 120000

static OSMutex https_mutex;
static int started;
static int table_started;
static int table_busy;
static unsigned long table_next;
static int table_asked;

static unsigned long now_ms(void)
{
	return (unsigned long)OSTicksToMilliseconds(OSGetSystemTime());
}

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

static int https_connect(const char *host, unsigned short port, unsigned long timeout_ms,
	console_https_socket *out)
{
	struct sockaddr_in target;
	unsigned long address, deadline = now_ms() + timeout_ms;
	int handle;

	address = (unsigned long)posix_resolve_ipv4(host);
	if (!address)
	{
		wiiu_log("https: %s did not resolve", host);
		return 0;
	}
	handle = posix_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (handle < 0)
		return 0;
	memset(&target, 0, sizeof(target));
	target.sin_family = AF_INET;
	target.sin_port = htons(port);
	target.sin_addr.s_addr = (in_addr_t)address;
	/* (not blocking, as the plain list's: the deadline bounds the connect
	too, and every send and receive waits on select) */
	posix_socket_set_nonblocking(handle, 1);
	if (posix_socket_connect(handle, &target, sizeof(target)) != 0)
	{
		int code = posix_socket_last_error();

		if ((code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) || !wait_ready(handle, 1, deadline))
		{
			wiiu_log("https: could not connect to %s (Winsock %d)", host, code);
			posix_socket_close(handle);
			return 0;
		}
	}
	*out = (console_https_socket)(long)handle;
	return 1;
}

static int https_send(console_https_socket socket, const unsigned char *data, int size, unsigned long timeout_ms)
{
	int handle = (int)(long)socket;
	unsigned long deadline = now_ms() + timeout_ms;

	for (;;)
	{
		int count;

		if (!wait_ready(handle, 1, deadline))
			return CONSOLE_HTTPS_IO_TIMEOUT;
		count = posix_socket_send(handle, data, size, 0);
		if (count > 0)
			return count;
		if (count < 0 && posix_socket_last_error() == WSAEWOULDBLOCK)
			continue;
		return -1;
	}
}

static int https_receive(console_https_socket socket, unsigned char *buffer, int size, unsigned long timeout_ms)
{
	int handle = (int)(long)socket;
	unsigned long deadline = now_ms() + timeout_ms;

	for (;;)
	{
		int count;

		if (!wait_ready(handle, 0, deadline))
			return CONSOLE_HTTPS_IO_TIMEOUT;
		count = posix_socket_recv(handle, buffer, size, 0);
		if (count >= 0)
			return count;
		switch (posix_socket_last_error())
		{
		case WSAEWOULDBLOCK:
			continue;
		/* (a server that resets the connection at the end, as a close) */
		case WSAECONNRESET:
			return 0;
		default:
			return -1;
		}
	}
}

static void https_close(console_https_socket socket)
{
	posix_socket_close((int)(long)socket);
}

/* wiiu_net.c's pool (above: Cafe OS's own source has no declaration in
wut) */
static int https_random(unsigned char *buffer, int size)
{
	posix_random_bytes(buffer, (posix_ulong)size);
	return 1;
}

static unsigned long https_milliseconds(void)
{
	return now_ms();
}

static void https_lock(void)
{
	OSLockMutex(&https_mutex);
}

static void https_unlock(void)
{
	OSUnlockMutex(&https_mutex);
}

static void https_log(const char *text)
{
	wiiu_log("%s", text);
}

static struct console_https_port wiiu_https_port =
{
	https_connect, https_send, https_receive, https_close, https_random, https_milliseconds,
	https_lock, https_unlock, https_log, "WarthogWiiU/1",
	/* (the pin document: plain HTTP from the list's host, wiiu_online.c;
	where it is kept is set at the start) */
	wiiu_online_plain_get, NULL,
};

void wiiu_https_start(void)
{
	int expected = 0;

	/* (the first caller sets it up: the list's thread, once the network is
	up) */
	if (!__atomic_compare_exchange_n(&started, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	OSInitMutex(&https_mutex);
	/* (the newest pin document, beside config.toml) */
	config_folder(pins_path, sizeof(pins_path) - 32);
	strcat(pins_path, "console_pins.signed");
	wiiu_https_port.pins_path = pins_path;
	console_https_initialize(&wiiu_https_port);
}

void wiiu_https_table_start(void)
{
	static int done;

	/* (the game's start, once: the cached table, and the game's network
	rules told of its layouts) */
	if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
		return;
	delta_legacy_start();
	__atomic_store_n(&table_started, 1, __ATOMIC_RELEASE);
}

void wiiu_https_table_refresh(void)
{
	unsigned long waited = 0;
	int expected = 0;

	if (!console_https_available() || (table_asked && (long)(now_ms() - table_next) < 0))
		return;
	/* (the first list comes during the game's start: the table's settings
	and its cache are read once the game has started) */
	while (!__atomic_load_n(&table_started, __ATOMIC_ACQUIRE))
	{
		if (waited >= TABLE_START_WAIT || thread_stop_sleep(250))
			return;
		waited += 250;
	}
	/* (one thread at a time: a later list's thread may be here already) */
	if (!__atomic_compare_exchange_n(&table_busy, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	table_asked = 1;
	table_next = now_ms() + (delta_legacy_refresh() ? TABLE_REFRESH : TABLE_RETRY);
	__atomic_store_n(&table_busy, 0, __ATOMIC_RELEASE);
}

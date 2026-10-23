/*
IFADDRS.H

The console's one network interface (port/wiiu/src/wiiu_net.c): its address
and mask from nn::ac, for system link's broadcast (posix_net.c).
*/

#ifndef __WIIU_IFADDRS_H
#define __WIIU_IFADDRS_H

#include <sys/socket.h>

struct ifaddrs
{
	struct ifaddrs *ifa_next;
	char *ifa_name;
	unsigned int ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	struct sockaddr *ifa_broadaddr;
	void *ifa_data;
};
#define ifa_dstaddr ifa_broadaddr

int getifaddrs(struct ifaddrs **addresses);
void freeifaddrs(struct ifaddrs *addresses);

#endif

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

struct loopback_ifaddrs {
	struct ifaddrs ifa;
	union {
		struct sockaddr sa;
		struct sockaddr_in v4;
		struct sockaddr_in6 v6;
	} address, netmask;
	char name[3];
};

static struct loopback_ifaddrs *loopback_entry(int family) {
	struct loopback_ifaddrs *entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	memcpy(entry->name, "lo", 3);
	entry->ifa.ifa_name = entry->name;
	entry->ifa.ifa_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
	entry->ifa.ifa_addr = &entry->address.sa;
	entry->ifa.ifa_netmask = &entry->netmask.sa;
	entry->address.sa.sa_family = family;
	entry->netmask.sa.sa_family = family;
	if (family == AF_INET) {
		entry->address.v4.sin_addr.s_addr = htonl(0x7f000001U);
		entry->netmask.v4.sin_addr.s_addr = htonl(0xff000000U);
	} else {
		entry->address.v6.sin6_addr = in6addr_loopback;
		memset(&entry->netmask.v6.sin6_addr, 0xff,
		    sizeof(entry->netmask.v6.sin6_addr));
	}
	return entry;
}

int getifaddrs(struct ifaddrs **ifap) {
	struct loopback_ifaddrs *v4, *v6;
	if (ifap == NULL) {
		errno = EFAULT;
		return -1;
	}
	*ifap = NULL;
	v4 = loopback_entry(AF_INET);
	v6 = loopback_entry(AF_INET6);
	if (v4 == NULL || v6 == NULL) {
		free(v4);
		free(v6);
		errno = ENOMEM;
		return -1;
	}
	v4->ifa.ifa_next = &v6->ifa;
	*ifap = &v4->ifa;
	return 0;
}

void freeifaddrs(struct ifaddrs *ifa) {
	while (ifa != NULL) {
		struct ifaddrs *next = ifa->ifa_next;
		free(ifa);
		ifa = next;
	}
}

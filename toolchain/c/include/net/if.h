#ifndef REGISTRY_NATIVE_C_INCLUDE_NET_IF_H
#define REGISTRY_NATIVE_C_INCLUDE_NET_IF_H

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

#ifndef IF_NAMESIZE
#define IF_NAMESIZE IFNAMSIZ
#endif

struct if_nameindex {
	unsigned int if_index;
	char *if_name;
};

unsigned int if_nametoindex(const char *name);
char *if_indextoname(unsigned int index, char *name);
struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *indexes);

/* Interface flag bits, values as in Linux <net/if.h> (netdevice(7)) and musl.
 * The runtime's getifaddrs() (see ifaddrs.h in this overlay) reports a
 * loopback-only interface set; ported tools (e.g. OpenSSH's BindInterface
 * handling in sshconnect.c and Match-address handling in readconf.c) test
 * IFF_UP / IFF_LOOPBACK on those entries. */
#ifndef IFF_UP
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#endif

#endif

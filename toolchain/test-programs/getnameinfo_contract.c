#define _GNU_SOURCE

#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static int check(int condition, const char *name) {
    printf("getnameinfo_%s=%s\n", name, condition ? "yes" : "no");
    return condition;
}

int main(int argc, char **argv) {
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    int ok = 1;
    int error;
	struct addrinfo hints, *addresses = NULL;
	struct ifaddrs *interfaces = NULL, *interface;
	int common_only = argc == 2 && strcmp(argv[1], "--common") == 0;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(514);
    inet_pton(AF_INET, "192.0.2.1", &sin.sin_addr);

    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), NULL, 0, 1 << 30);
    ok &= check(error == EAI_BADFLAGS, "badflags");
#ifdef NI_NUMERICSCOPE
    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), NULL, 0, NI_NUMERICSCOPE | NI_NUMERICHOST);
#ifdef __GLIBC__
    ok &= check(error == EAI_BADFLAGS, "glibc_rejects_numericscope_flag");
#else
    ok &= check(error == 0 && strcmp(host, "192.0.2.1") == 0,
        "musl_accepts_numericscope_flag");
#endif
#endif

    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), NULL, 0,
        NULL, 0, 0);
    ok &= check(error == 0, "both_outputs_absent");

    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host, 0,
        NULL, 0, NI_NUMERICHOST);
    ok &= check(error == 0, "zero_host_length");

    memset(host, 'X', sizeof(host));
    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), NULL, 0, 0);
    ok &= check(error == 0 && strcmp(host, "192.0.2.1") == 0,
        "ptr_miss_prefilled_numeric_fallback");

    memset(host, 'X', sizeof(host));
    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
    ok &= check(error == EAI_NONAME && host[0] == 'X',
        "ptr_miss_namereqd");

    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), service, sizeof(service), NI_NUMERICHOST);
    ok &= check(error == 0 && strcmp(host, "192.0.2.1") == 0 &&
        strcmp(service, "shell") == 0, "tcp_service");

    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), NULL, 0,
        service, sizeof(service), NI_DGRAM);
    ok &= check(error == 0 && strcmp(service, "syslog") == 0,
        "udp_service");

    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), NULL, 0,
        service, sizeof(service), NI_NUMERICSERV);
    ok &= check(error == 0 && strcmp(service, "514") == 0,
        "numeric_service");

	/* smtp is intentionally outside the old hardcoded OpenSSH-only table. */
	sin.sin_port = htons(25);
	error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), NULL, 0,
	    service, sizeof(service), 0);
	ok &= check(error == 0 && strcmp(service, "smtp") == 0,
	    "services_database");

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_scope_id = 1;
    inet_pton(AF_INET6, "fe80::1", &sin6.sin6_addr);
    error = getnameinfo((const struct sockaddr *)&sin6, sizeof(sin6), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    ok &= check(error == 0 && strcmp(host, "fe80::1%lo") == 0,
        "ipv6_named_scope");
#ifdef NI_NUMERICSCOPE
    error = getnameinfo((const struct sockaddr *)&sin6, sizeof(sin6), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST | NI_NUMERICSCOPE);
    ok &= check(error == 0 && strcmp(host, "fe80::1%1") == 0,
        "ipv6_forced_numeric_scope");
#endif

    sin6.sin6_scope_id = 424242;
    error = getnameinfo((const struct sockaddr *)&sin6, sizeof(sin6), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    ok &= check(error == 0 && strcmp(host, "fe80::1%424242") == 0,
        "ipv6_unknown_scope_numeric_fallback");

    sin6.sin6_scope_id = 1;
    inet_pton(AF_INET6, "2001:db8::1", &sin6.sin6_addr);
    error = getnameinfo((const struct sockaddr *)&sin6, sizeof(sin6), host,
        sizeof(host), NULL, 0, NI_NUMERICHOST);
    ok &= check(error == 0 && strcmp(host, "2001:db8::1%1") == 0,
        "ipv6_global_scope_stays_numeric");

    ok &= check(if_nametoindex("lo") == 1, "loopback_name_to_index");
    {
        char interface[IF_NAMESIZE];
        ok &= check(if_indextoname(1, interface) != NULL &&
            strcmp(interface, "lo") == 0, "loopback_index_to_name");
    }
	{
		int found_v4 = 0, found_v6 = 0;
		error = getifaddrs(&interfaces);
		for (interface = interfaces; error == 0 && interface != NULL;
		    interface = interface->ifa_next) {
			if (interface->ifa_name == NULL || interface->ifa_addr == NULL ||
			    strcmp(interface->ifa_name, "lo") != 0 ||
			    (interface->ifa_flags & (IFF_UP | IFF_LOOPBACK | IFF_RUNNING)) !=
			    (IFF_UP | IFF_LOOPBACK | IFF_RUNNING))
				continue;
			if (interface->ifa_addr->sa_family == AF_INET &&
			    ((struct sockaddr_in *)interface->ifa_addr)->sin_addr.s_addr ==
			    htonl(0x7f000001U))
				found_v4 = 1;
			if (interface->ifa_addr->sa_family == AF_INET6 &&
			    IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)
			    interface->ifa_addr)->sin6_addr))
				found_v6 = 1;
		}
		ok &= check(error == 0 && found_v4 && found_v6,
		    "loopback_getifaddrs");
		freeifaddrs(interfaces);
	}
	{
		struct if_nameindex *indexes = if_nameindex();
		int found = 0;
		struct if_nameindex *index;
		for (index = indexes; index != NULL && index->if_index != 0; index++)
			if (index->if_index == 1 && index->if_name != NULL &&
			    strcmp(index->if_name, "lo") == 0)
				found = 1;
		ok &= check(found, "loopback_nameindex_list");
		if_freenameindex(indexes);
	}

	if (!common_only) {
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_CANONNAME;
	error = getaddrinfo("aliases", "ssh", &hints, &addresses);
	ok &= check(error == 0 && addresses != NULL &&
	    addresses->ai_family == AF_INET &&
	    ntohs(((struct sockaddr_in *)addresses->ai_addr)->sin_port) == 22 &&
	    memcmp(&((struct sockaddr_in *)addresses->ai_addr)->sin_addr,
	    &(struct in_addr){.s_addr = htonl(0xcb007109)},
	    sizeof(struct in_addr)) == 0 && addresses->ai_canonname != NULL &&
	    strcmp(addresses->ai_canonname, "custombox.example.test") == 0,
	    "forward_hosts_alias_service_canonname");
	freeaddrinfo(addresses);
	addresses = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo("scopedbox", "ssh", &hints, &addresses);
	{
		int scoped_forward_ok = error == 0 && addresses != NULL &&
		    addresses->ai_family == AF_INET6 &&
		    ((struct sockaddr_in6 *)addresses->ai_addr)->sin6_scope_id == 1 &&
		    ntohs(((struct sockaddr_in6 *)addresses->ai_addr)->sin6_port) == 22;
#ifdef __GLIBC__
		check(scoped_forward_ok, "forward_scoped_hosts");
#else
		ok &= check(scoped_forward_ok, "forward_scoped_hosts");
#endif
	}
	freeaddrinfo(addresses);
	addresses = NULL;
	}

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
    ok &= check(error == 0 && strcmp(host, "127.0.0.1") != 0,
        "ptr_loopback_reverse");

    if (!common_only) {
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    inet_pton(AF_INET, "203.0.113.9", &sin.sin_addr);
    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
    ok &= check(error == 0 && strcmp(host, "custombox.example.test") == 0,
        "live_hosts_precedes_dns");
    error = getnameinfo((const struct sockaddr *)&sin, sizeof(sin), host,
        sizeof(host), NULL, 0, NI_NAMEREQD | NI_NOFQDN);
    ok &= check(error == 0 && strcmp(host, "custombox.example.test") == 0,
        "musl_nofqdn_noop");

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "::ffff:203.0.113.9", &sin6.sin6_addr);
    error = getnameinfo((const struct sockaddr *)&sin6, sizeof(sin6), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
#ifdef __GLIBC__
    check(error == 0 && strcmp(host, "custombox.example.test") == 0,
        "mapped_ipv6_live_hosts");
#else
    ok &= check(error == 0 && strcmp(host, "custombox.example.test") == 0,
        "mapped_ipv6_live_hosts");
#endif

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_scope_id = 1;
    inet_pton(AF_INET6, "fe80::1234", &sin6.sin6_addr);
    error = getnameinfo((const struct sockaddr *)&sin6, sizeof(sin6), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
#ifdef __GLIBC__
    check(error == 0 && strcmp(host, "scopedbox") == 0,
        "scoped_hosts_match");
#else
    ok &= check(error == 0 && strcmp(host, "scopedbox") == 0,
        "scoped_hosts_match");
#endif
    sin6.sin6_scope_id = 2;
    memset(host, 'X', sizeof(host));
    error = getnameinfo((const struct sockaddr *)&sin6, sizeof(sin6), host,
        sizeof(host), NULL, 0, NI_NAMEREQD);
#ifdef __GLIBC__
    check(error == EAI_NONAME && host[0] == 'X', "scoped_hosts_mismatch");
#else
    ok &= check(error == EAI_NONAME && host[0] == 'X',
        "scoped_hosts_mismatch");
#endif
	}

    printf("getnameinfo_contract=%s\n", ok ? "ok" : "fail");
    return ok ? 0 : 1;
}

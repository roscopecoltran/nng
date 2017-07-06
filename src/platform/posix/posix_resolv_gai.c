//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//


#include "core/nng_impl.h"

#ifdef NNG_USE_POSIX_RESOLV_GAI
#include "platform/posix/posix_aio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>


// We use a single resolver taskq - but we allocate a few threads
// for it to ensure that names can be looked up concurrently.  This isn't
// as elegant or scaleable as a true asynchronous resolver would be, but
// it has the advantage of being fairly portable, and concurrent enough for
// the vast, vast majority of use cases.  The total thread count can be
// changed with this define.  Note that some platforms may not have a
// thread-safe getaddrinfo().  In that case they should set this to 1.

#ifndef NNG_POSIX_RESOLV_CONCURRENCY
#define NNG_POSIX_RESOLV_CONCURRENCY    4
#endif


static nni_taskq *nni_posix_resolv_tq = NULL;
static nni_mtx nni_posix_resolv_mtx;

typedef struct nni_posix_resolv_item   nni_posix_resolv_item;
struct nni_posix_resolv_item {
	int		family;
	int		passive;
	const char *	name;
	const char *	serv;
	int		proto;
	nni_aio *	aio;
	nni_taskq_ent	tqe;
};


static void
nni_posix_resolv_finish(nni_posix_resolv_item *item, int rv)
{
	nni_aio *aio = item->aio;

	aio->a_prov_data = NULL;
	nni_aio_finish(aio, rv, 0);
	NNI_FREE_STRUCT(item);
}


static void
nni_posix_resolv_cancel(nni_aio *aio)
{
	nni_posix_resolv_item *item;

	nni_mtx_lock(&nni_posix_resolv_mtx);
	if ((item = aio->a_prov_data) == NULL) {
		nni_mtx_unlock(&nni_posix_resolv_mtx);
		return;
	}
	aio->a_prov_data = NULL;
	nni_mtx_unlock(&nni_posix_resolv_mtx);
	nni_taskq_cancel(nni_posix_resolv_tq, &item->tqe);
	NNI_FREE_STRUCT(item);
}


static int
nni_posix_gai_errno(int rv)
{
	switch (rv) {
	case 0:
		return (0);

	case EAI_MEMORY:
		return (NNG_ENOMEM);

	case EAI_SYSTEM:
		return (nni_plat_errno(errno));

	case EAI_NONAME:
	case EAI_SERVICE:
		return (NNG_EADDRINVAL);

	case EAI_BADFLAGS:
		return (NNG_EINVAL);

	case EAI_SOCKTYPE:
		return (NNG_ENOTSUP);

	default:
		return (NNG_ESYSERR);
	}
}


static void
nni_posix_resolv_task(void *arg)
{
	nni_posix_resolv_item *item = arg;
	nni_aio *aio = item->aio;
	struct addrinfo hints;
	struct addrinfo *results;
	struct addrinfo *probe;
	int i, rv;

	results = NULL;

	switch (item->family) {
	case AF_INET:
	case AF_INET6:
	case AF_UNSPEC:
		// We treat these all as IP addresses.  The service and the
		// host part are split.
		memset(&hints, 0, sizeof (hints));
		if (item->passive) {
			hints.ai_flags |= AI_PASSIVE;
		}
#ifdef AI_ADDRCONFIG
		hints.ai_flags |= AI_ADDRCONFIG;
#endif
		hints.ai_protocol = item->proto;
		hints.ai_family = item->family;
		if (item->family == AF_INET6) {
			// We prefer to have v4mapped addresses if a remote
			// v4 address isn't avaiable.  And we prefer to only
			// do this if we actually support v6.
#if defined(AI_V4MAPPED_CFG)
			hints.ai_flags |= AI_V4MAPPED_CFG;
#elif defined(AI_V4MAPPED)
			hints.ai_flags |= AI_V4MAPPED;
#endif
		}

		rv = getaddrinfo(item->name, item->serv, &hints, &results);
		if (rv != 0) {
			rv = nni_posix_gai_errno(rv);
			break;
		}

		// Count the total number of results.
		aio->a_naddrs = 0;
		for (probe = results; probe != NULL; probe = probe->ai_next) {
			// Only count v4 and v6 addresses.
			switch (probe->ai_addr->sa_family) {
			case AF_INET:
			case AF_INET6:
				aio->a_naddrs++;
				break;
			}
		}
		// If the only results were not IPv4 or IPv6...
		if (aio->a_naddrs == 0) {
			rv = NNG_EADDRINVAL;
			break;
		}
		aio->a_addrs = NNI_ALLOC_STRUCTS(aio->a_addrs, aio->a_naddrs);
		if (aio->a_addrs == NULL) {
			aio->a_naddrs = 0;
			rv = NNG_ENOMEM;
			break;
		}
		i = 0;
		for (probe = results; probe != NULL; probe = probe->ai_next) {
			struct sockaddr_in *sin;
			struct sockaddr_in6 *sin6;
			nng_sockaddr *sa = &aio->a_addrs[i];

			switch (probe->ai_addr->sa_family) {
			case AF_INET:
				sin = (void *) probe->ai_addr;
				sa->s_un.s_in.sa_family = NNG_AF_INET;
				sa->s_un.s_in.sa_port = sin->sin_port;
				sa->s_un.s_in.sa_addr = sin->sin_addr.s_addr;
				i++;
				break;
			case AF_INET6:
				sin6 = (void *) probe->ai_addr;
				sa->s_un.s_in6.sa_family = NNG_AF_INET6;
				sa->s_un.s_in6.sa_port = sin6->sin6_port;
				memcpy(sa->s_un.s_in6.sa_addr,
				    sin6->sin6_addr.s6_addr, 16);
				i++;
				break;
			default:
				// Other address types are ignored.
				break;
			}
		}
		// Resolution complete!
		rv = 0;
		break;

	default:
		// Some other family requested we don't understand.
		rv = NNG_ENOTSUP;
		break;
	}

done:
	if (results != NULL) {
		freeaddrinfo(results);
	}
	nni_mtx_lock(&nni_posix_resolv_mtx);
	nni_posix_resolv_finish(item, rv);
	nni_mtx_unlock(&nni_posix_resolv_mtx);
}


static void
nni_posix_resolv_ip(const char *host, const char *serv, int passive,
    int family, int proto, nni_aio *aio)
{
	nni_posix_resolv_item *item;
	int rv;

	if ((aio->a_naddrs != 0) && (aio->a_addrs != NULL)) {
		NNI_FREE_STRUCTS(aio->a_addrs, aio->a_naddrs);
	}
	if ((item = NNI_ALLOC_STRUCT(item)) == NULL) {
		nni_aio_finish(aio, NNG_ENOMEM, 0);
		return;
	}

	nni_taskq_ent_init(&item->tqe, nni_posix_resolv_task, item);

	switch (family) {
	case NNG_AF_INET:
		item->family = AF_INET;
		break;
	case NNG_AF_INET6:
		item->family = AF_INET6;
		break;
	case NNG_AF_UNSPEC:
		item->family = AF_UNSPEC;
		break;
	}
	// NB: host and serv must remain valid until this is completed.
	item->passive = passive;
	item->name = host;
	item->serv = serv;
	item->proto = proto;
	item->aio = aio;

	nni_mtx_lock(&nni_posix_resolv_mtx);
	// If we were stopped, we're done...
	if ((rv = nni_aio_start(aio, nni_posix_resolv_cancel, item)) != 0) {
		nni_mtx_unlock(&nni_posix_resolv_mtx);
		NNI_FREE_STRUCT(item);
		return;
	}
	if ((rv = nni_taskq_dispatch(nni_posix_resolv_tq, &item->tqe)) != 0) {
		nni_posix_resolv_finish(item, rv);
		nni_mtx_unlock(&nni_posix_resolv_mtx);
		return;
	}
	nni_mtx_unlock(&nni_posix_resolv_mtx);
}


void
nni_plat_tcp_resolv(const char *host, const char *serv, int family,
    int passive, nni_aio *aio)
{
	nni_posix_resolv_ip(host, serv, passive, family, IPPROTO_TCP, aio);
}


int
nni_posix_resolv_sysinit(void)
{
	int rv;

	if ((rv = nni_mtx_init(&nni_posix_resolv_mtx)) != 0) {
		return (rv);
	}
	if ((rv = nni_taskq_init(&nni_posix_resolv_tq, 4)) != 0) {
		nni_mtx_fini(&nni_posix_resolv_mtx);
		return (rv);
	}
	return (0);
}


void
nni_posix_resolv_sysfini(void)
{
	if (nni_posix_resolv_tq != NULL) {
		nni_taskq_fini(nni_posix_resolv_tq);
		nni_posix_resolv_tq = NULL;
	}
	nni_mtx_fini(&nni_posix_resolv_mtx);
}


#else

// Suppress empty symbols warnings in ranlib.
int nni_posix_resolv_gai_not_used = 0;

#endif // NNG_USE_POSIX_RESOLV_GAI
//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#include <stdio.h>
#include <string.h>

// For now the list of transports is hard-wired.  Adding new transports
// to the system dynamically is something that might be considered later.
extern nni_tran nni_inproc_tran;
extern nni_tran nni_tcp_tran;
extern nni_tran nni_ipc_tran;

typedef struct nni_transport {
	nni_tran      t_tran;
	char          t_prefix[16]; // e.g. "tcp://" or "tls+tcp://"
	nni_list_node t_node;
} nni_transport;

static nni_list nni_tran_list;
static nni_mtx  nni_tran_lk;
static int      nni_tran_inited;

int
nni_tran_register(const nni_tran *tran)
{
	nni_transport *t;
	int            rv;

	// Its entirely possible that we are called before any sockets
	// are opened.  Make sure we are initialized.  This has to be
	// protected by a guard to prevent infinite recursion, since
	// nni_init also winds up calling us.
	if (!nni_tran_inited) {
		nni_init();
	}

	if (tran->tran_version != NNI_TRANSPORT_VERSION) {
		return (NNG_ENOTSUP);
	}

	nni_mtx_lock(&nni_tran_lk);
	// Check to see if the transport is already registered...
	NNI_LIST_FOREACH (&nni_tran_list, t) {
		if (strcmp(tran->tran_scheme, t->t_tran.tran_scheme) == 0) {
			nni_mtx_unlock(&nni_tran_lk);
			return (NNG_ESTATE);
		}
	}
	if ((t = NNI_ALLOC_STRUCT(t)) == NULL) {
		return (NNG_ENOMEM);
	}

	t->t_tran = *tran;
	(void) snprintf(
	    t->t_prefix, sizeof(t->t_prefix), "%s://", tran->tran_scheme);
	if ((rv = t->t_tran.tran_init()) != 0) {
		nni_mtx_unlock(&nni_tran_lk);
		NNI_FREE_STRUCT(t);
		return (rv);
	}
	nni_list_append(&nni_tran_list, t);
	nni_mtx_unlock(&nni_tran_lk);
	return (0);
}

nni_tran *
nni_tran_find(const char *addr)
{
	// address is of the form "<scheme>://blah..."
	nni_transport *t;

	nni_mtx_lock(&nni_tran_lk);
	NNI_LIST_FOREACH (&nni_tran_list, t) {
		if (strncmp(addr, t->t_prefix, strlen(t->t_prefix)) == 0) {
			nni_mtx_unlock(&nni_tran_lk);
			return (&t->t_tran);
		}
	}
	nni_mtx_unlock(&nni_tran_lk);
	return (NULL);
}

int
nni_tran_chkopt(int o, const void *v, size_t sz)
{
	nni_transport *t;
	int            rv = NNG_ENOTSUP;
	nni_mtx_lock(&nni_tran_lk);
	NNI_LIST_FOREACH (&nni_tran_list, t) {
		int x;
		if (t->t_tran.tran_chkopt == NULL) {
			continue;
		}
		if ((x = t->t_tran.tran_chkopt(o, v, sz)) != NNG_ENOTSUP) {
			if ((rv = x) != 0) {
				break;
			}
		}
	}
	nni_mtx_unlock(&nni_tran_lk);
	return (rv);
}

// nni_tran_sys_init initializes the entire transport subsystem, including
// each individual transport.
int
nni_tran_sys_init(void)
{
	int rv;

	nni_tran_inited = 1;
	NNI_LIST_INIT(&nni_tran_list, nni_transport, t_node);
	nni_mtx_init(&nni_tran_lk);

	if (((rv = nni_tran_register(&nni_inproc_tran)) != 0) ||
	    ((rv = nni_tran_register(&nni_ipc_tran)) != 0) ||
	    ((rv = nni_tran_register(&nni_tcp_tran)) != 0)) {
		nni_tran_sys_fini();
		return (rv);
	}
	return (0);
}

// nni_tran_sys_fini finalizes the entire transport system, including all
// transports.
void
nni_tran_sys_fini(void)
{
	nni_transport *t;

	while ((t = nni_list_first(&nni_tran_list)) != NULL) {
		nni_list_remove(&nni_tran_list, t);
		t->t_tran.tran_fini();
		NNI_FREE_STRUCT(t);
	}
	nni_mtx_fini(&nni_tran_lk);
	nni_tran_inited = 0;
}

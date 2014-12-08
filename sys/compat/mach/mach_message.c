/*-
 * Copyright (c) 2002-2003 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Emmanuel Dreyfus
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_compat_mach.h" /* For COMPAT_MACH in <sys/ktrace.h> */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/signal.h>
#include <sys/uio.h>
#include <sys/ktrace.h>

#include <vm/uma.h>
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>

#include <compat/mach/mach_clock.h>
#include <compat/mach/mach_exec.h>
#include <compat/mach/mach_message.h>
#include <compat/mach/mach_port.h>
#include <compat/mach/mach_proto.h>
#include <compat/mach/mach_types.h>
#include <compat/mach/mach_vm.h>

#ifdef COMPAT_DARWIN
#include <compat/darwin/darwin_exec.h>
#endif

/* Mach message zone */
static uma_zone_t mach_message_zone;

static inline int mach_msg_send(struct thread *, mach_msg_header_t *, int *, size_t);
static inline int mach_msg_recv(struct thread *, mach_msg_header_t *,
    int, size_t, unsigned int, mach_port_name_t);
static inline
    struct thread *mach_get_target_task(struct thread *, struct mach_port *);
static inline void mach_drop_rights(struct mach_right *, int);
static inline
    void mach_trade_rights(struct thread *, struct thread *, mach_port_t *, int);
static inline
    int mach_trade_rights_complex(struct thread *, struct mach_message *);

int
sys_mach_msg_overwrite_trap(struct thread *td, struct mach_msg_overwrite_trap_args *uap)
{
	/* {
		syscallarg(mach_msg_header_t *) msg;
		syscallarg(mach_msg_option_t) option;
		syscallarg(mach_msg_size_t) send_size;
		syscallarg(mach_msg_size_t) rcv_size;
		syscallarg(mach_port_name_t) rcv_name;
		syscallarg(mach_msg_timeout_t) timeout;
		syscallarg(mach_port_name_t) notify;
		syscallarg(mach_msg_header_t *) rcv_msg;
		syscallarg(mach_msg_size_t) scatter_list_size;
	} */
	size_t send_size, recv_size;
	mach_msg_header_t *msg;
	int error;
	mach_msg_option_t option = uap->option;

	DPRINTF(("mach_msg_overwrite_trap("
		   "td=%p, msg=%p, option=%d, send_size=%u, rcv_size=%u, \n"
		   "rcv_name=%d, timeout=%u, notify=%d, rcv_msg=%p, list_size=%u)\n",
		   td, uap->msg, uap->option, uap->send_size, uap->rcv_size, uap->rcv_name,
		   uap->timeout, uap->notify, uap->rcv_msg, uap->scatter_list_size));
	send_size = uap->send_size;
	recv_size = uap->rcv_size;
	error = 0;


	option &= MACH_MSG_OPTION_USER;
	/* XXX not safe enough: lots of big messages will kill us */
	if (send_size > MACH_MAX_MSG_LEN) {
		td->td_retval[0] = MACH_SEND_TOO_LARGE;
		return (0);
	}
	if (recv_size > MACH_MAX_MSG_LEN) {
		td->td_retval[0] = MACH_RCV_TOO_LARGE;
		return (0);
	}

	/*
	 * Two options: receive or send. If both are
	 * set, we must send, and then receive. If
	 * send fail, then we skip recieve.
	 */
	msg = uap->msg;
	if (option & MACH_SEND_MSG)
		error = mach_msg_send(td, msg, &option, send_size);

	if ((option & MACH_RCV_MSG) && (error == MACH_MSG_SUCCESS)) {
		/*
		 * Find a buffer for the reply.
		 */
		if (uap->rcv_msg != NULL)
			msg = uap->rcv_msg;
		else if (uap->msg != NULL)
			msg = uap->msg;
		else {
			td->td_retval[0] = MACH_RCV_INVALID_DATA;
			return (0);
		}

		error = mach_msg_recv(td, msg, option, recv_size,
		    uap->timeout, uap->rcv_name);
	}

	td->td_retval[0] = error;
	return (0);
}

/*
 * Send a Mach message. This returns a Mach message error code.
 */
static inline int
mach_msg_send(struct thread *td, mach_msg_header_t *msg, int *option, size_t send_size)
{
	struct mach_emuldata *med;
	struct mach_port *mp;
	struct proc *p = td->td_proc;
	mach_msg_header_t *sm;
	struct mach_service *srv;
	mach_port_t ln;
	mach_port_t rn;
	struct mach_right *lr = NULL;
	struct mach_right *rr;
	int rights;
	int bits;
	int ret;
	size_t reply_size;
	int error = 0;

	if (msg == NULL)
		return (MACH_SEND_INVALID_DATA);

	/*
	 * Allocate memory for the message and its reply,
	 * and copy the whole message in the kernel.
	 */
	sm = malloc(send_size, M_MACH, M_WAITOK);
	if ((error = copyin(msg, sm, send_size)) != 0) {
		ret = MACH_SEND_INVALID_DATA;
		goto out1;
	}

	/* Dump the Mach message */
	ktrmmsg((char *)sm, send_size);

	/*
	 * Handle rights in the message
	 */
	ln = sm->msgh_local_port;
	rn = sm->msgh_remote_port;

	DPRINTF(("mach_msg_send: local_port=%p remote_port=%p",
		   ln, rn));
	if (ln)
		lr = mach_right_check(ln, td, MACH_PORT_TYPE_ALL_RIGHTS);
	rr = mach_right_check(rn, td, MACH_PORT_TYPE_ALL_RIGHTS);
	if ((rr == NULL) || (rr->mr_port == NULL)) {
#ifdef DEBUG_MACH
		printf("msg id %d: invalid dest\n", sm->msgh_id);
#endif
		ret = MACH_SEND_INVALID_DEST;
		goto out1;
	}

	/*
	 * Check that the process has a send right on
	 * the remote port.
	 */
	rights = (MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_SEND_ONCE);
	if (mach_right_check(rn, td, rights) == NULL) {
		ret = MACH_SEND_INVALID_RIGHT;
		goto out1;
	}

	/*
	 * If the remote port is a special port (host, kernel,
	 * clock, or io_master), the message will be handled
	 * by the kernel.
	 */
	med = (struct mach_emuldata *)p->p_emuldata;
	mp = rr->mr_port;
	if (mp->mp_flags & MACH_MP_INKERNEL) {
		struct mach_trap_args args;
		mach_msg_header_t *rm;
		size_t min_reqlen, max_replen;

		/*
		 * Look for the function that will handle it,
		 * using the message id.
		 */
		for (srv = mach_services_table; srv->srv_id; srv++)
			if (srv->srv_id == sm->msgh_id)
				break;

		/*
		 * If no match, give up, and display a warning.
		 */
		if (srv->srv_handler == NULL) {
			uprintf("No mach server for id = %d\n",
			    sm->msgh_id);
			ret = MACH_SEND_INVALID_DEST;
			goto out1;
		}
		min_reqlen = srv->srv_reqlen;
		max_replen = srv->srv_replen;

		/*
		 * Special case when the kernel behaves as
		 * the client: replies to exceptions and
		 * notifications. There will be no reply,
		 * as we already receive a reply.
		 * - request and reply are swapped
		 * - there will be no reply, so set lr to NULL.
		 * - skip the lr == NULL tests
		 * XXX This is inelegant.
		 */
		if ((sm->msgh_id >= 2501) && (sm->msgh_id <= 2503)) {
			min_reqlen = srv->srv_replen;
			max_replen = srv->srv_reqlen;
			lr = NULL;
			goto skip_null_lr;
		}

		/*
		 * Check that the local port is valid, else
		 * we will not be able to send the reply
		 */
		if ((lr == NULL) ||
		    (lr->mr_port == NULL) ||
		    (lr->mr_port->mp_recv == NULL)) {
#ifdef DEBUG_MACH
			printf("msg id %d: invalid src\n", sm->msgh_id);
#endif
			ret = MACH_SEND_INVALID_REPLY;
			goto out1;
		}
skip_null_lr:

		/*
		 * Sanity check message length. We do not want the
		 * server to:
		 * 1) use kernel memory located after
		 *    the end of the request message.
		 */
		if (send_size < min_reqlen) {
#ifdef DEBUG_MACH
			printf("mach server %s: smsg overflow: "
			    "send = %lu, min = %lu\n",
			    srv->srv_name, send_size, min_reqlen);
#endif
			ret = MACH_SEND_MSG_TOO_SMALL;
			goto out1;
		}

		/*
		 * 2) Overwrite kernel memory after the end of the
		 *    reply message buffer. This check is the
		 *    responsibility of the server.
		 */


		/*
		 * Invoke the server. We give it the opportunity
		 * to shorten recv_size if there is less data in
		 * the reply than what the sender expected.
		 * If lr is NULL, this is a no reply operation.
		 */
		reply_size = max_replen;
		if (lr != NULL)
			rm = malloc(reply_size, M_MACH, M_WAITOK | M_ZERO);
		else
			rm = NULL;

		args.td = td;
		args.ttd = mach_get_target_task(td, mp);
		args.smsg = sm;
		args.rmsg = rm;
		args.rsize = &reply_size;
		args.ssize = send_size;
		if ((ret = (*srv->srv_handler)(&args)) != 0)
			goto out1;

		/*
		 * No-reply opration: everything is done.
		 * Change option so that we skip the
		 * receive stage.
		 */
		if (lr == NULL) {
			*option &= ~MACH_RCV_MSG;
			return (MACH_MSG_SUCCESS);
		}

#ifdef DIAGNOSTIC
		/*
		 * Catch potential bug in the server (sanity
		 * check #2): did it output a larger message
		 * then the one that was allocated?
		 */
		if ((*option & MACH_RCV_MSG) && (reply_size > max_replen)) {
			uprintf("mach_msg: reply too big in %s\n",
			    srv->srv_name);
		}
#endif

		/*
		 * Queue the reply.
		 */
		mp = lr->mr_port;
		(void)mach_message_get(rm, reply_size, mp, NULL);
#ifdef DEBUG_MACH_MSG
		printf("pid %d: message queued on port %p (%d) [%p]\n",
		    p->p_pid, mp, rm->msgh_id,
		    mp->mp_recv->mr_sethead);
		if (sm->msgh_id == 404)
			printf("*** msg to bootstrap. port = %p, "
			    "recv = %p [%p]\n", mach_bootstrap_port,
			    mach_bootstrap_port->mp_recv,
			    mach_bootstrap_port->mp_recv->mr_sethead);
#endif
		wakeup(mp->mp_recv->mr_sethead);
		ret = MACH_MSG_SUCCESS;
out1:
		free(sm, M_MACH);

		return (ret);
	}

	/*
	 * The message is not to be handled by the kernel.
	 * Check that there is a valid receiver, and
	 * queue the message in the remote port.
	 */
	mp = rr->mr_port; /* (mp != NULL) already checked */
	if (mp->mp_recv == NULL) {
#ifdef DEBUG_MACH
		printf("msg id %d: invalid dst\n", sm->msgh_id);
#endif
		free(sm, M_MACH);
		return (MACH_SEND_INVALID_DEST);
	}

	(void)mach_message_get(sm, send_size, mp, td);
#ifdef DEBUG_MACH_MSG
	printf("pid %d: message queued on port %p (%d) [%p]\n",
	    p->p_pid, mp, sm->msgh_id,
	    mp->mp_recv->mr_sethead);
#endif
	/*
	 * Drop any right carried by the message.
	 */
	if (lr != NULL) {
		bits = MACH_MSGH_LOCAL_BITS(sm->msgh_bits);
		mach_drop_rights(lr, bits);
	}

	if (rr != NULL) {
		bits = MACH_MSGH_REMOTE_BITS(sm->msgh_bits);
		mach_drop_rights(rr, bits);
	}

	/*
	 * Wakeup any process awaiting for this message.
	 */
	wakeup(mp->mp_recv->mr_sethead);

	return (MACH_MSG_SUCCESS);
}

/*
 * Receive a Mach message. This returns a Mach message error code.
 */
static inline int
mach_msg_recv(struct thread *td, mach_msg_header_t *urm, int option, size_t recv_size, unsigned int timeout, mach_port_name_t mn)
{
	struct mach_port *mp;
#if defined(DEBUG_MACH_MSG) || defined(KTRACE)
	struct proc *p = td->td_proc;
#endif
	struct mach_message *mm;
	mach_port_t tmp;
	struct mach_right *cmr;
	struct mach_right *mr;
	int bits;
	int ret;
	int error = 0;

	mp = NULL;

	if (option & MACH_RCV_TIMEOUT)
		timeout = timeout * hz / 1000;
	else
		timeout = 0;

	/*
	 * Check for receive right on the port.
	 */
	mr = mach_right_check(mn, td, MACH_PORT_TYPE_RECEIVE);
	if (mr == NULL) {

		/*
		 * Is it a port set?
		 */
		mr = mach_right_check(mn, td, MACH_PORT_TYPE_PORT_SET);
		if (mr == NULL)
			return (MACH_RCV_INVALID_NAME);

		/*
		 * This is a port set. For each port in the
		 * port set, check we have receive right, and
		 * and check if we have some message.
		 */
		LIST_FOREACH(cmr, &mr->mr_set, mr_setlist) {
			if ((mach_right_check(cmr->mr_name, td,
			    MACH_PORT_TYPE_RECEIVE)) == NULL)
				return (MACH_RCV_INVALID_NAME);

			mp = cmr->mr_port;
#ifdef DEBUG_MACH
			if (mp->mp_recv != cmr)
				uprintf("mach_msg_trap: bad receive "
				    "port/right\n");
#endif
			if (mp->mp_count != 0)
				break;
		}

		/*
		 * If cmr is NULL then we found no message on
		 * any port. Sleep on the port set until we get
		 * some or until we get a timeout.
		 */
		if (cmr == NULL) {
#ifdef DEBUG_MACH_MSG
			printf("pid %d: wait on port %p [%p]\n",
			    p->p_pid, mp, mr->mr_sethead);
#endif
			mtx_lock(&mr->mr_lock);
			error = msleep(mr->mr_sethead, &mr->mr_lock, PZERO|PCATCH,
							   "mach_msg", timeout);
			mtx_unlock(&mr->mr_lock);
			if ((error == ERESTART) || (error == EINTR))
				return (MACH_RCV_INTERRUPTED);

			/*
			 * Check we did not loose the receive right
			 * while we were sleeping.
			 */
			if ((mach_right_check(mn, td,
			     MACH_PORT_TYPE_PORT_SET)) == NULL)
				return (MACH_RCV_PORT_DIED);

			/*
			 * Is there any pending message for
			 * a port in the port set?
			 */
			LIST_FOREACH(cmr, &mr->mr_set, mr_setlist) {
				mp = cmr->mr_port;
				if (mp->mp_count != 0)
					break;
			}

			if (cmr == NULL)
				return (MACH_RCV_TIMED_OUT);
		}

		/*
		 * We found a port with a pending message.
		 */
		mp = cmr->mr_port;

	} else {
		/*
		 * This is a receive on a simple port (no port set).
		 * If there is no message queued on the port,
		 * block until we get some.
		 */
		mp = mr->mr_port;

#ifdef DEBUG_MACH
		if (mp->mp_recv != mr)
			uprintf("mach_msg_trap: bad receive "
			    "port/right\n");
#endif
#ifdef DEBUG_MACH_MSG
		printf("pid %d: wait on port %p [%p]\n",
		    p->p_pid, mp, mr->mr_sethead);
#endif
		if (mp->mp_count == 0) {
			mtx_lock(&mr->mr_lock);
			error = msleep(mr->mr_sethead, &mr->mr_lock, PZERO|PCATCH,
						   "mach_msg", timeout);
			mtx_unlock(&mr->mr_lock);
			if ((error == ERESTART) || (error == EINTR))
				return (MACH_RCV_INTERRUPTED);

			/*
			 * Check we did not lose the receive right
			 * while we were sleeping.
			 */
			if ((mach_right_check(mn, td,
			     MACH_PORT_TYPE_RECEIVE)) == NULL)
				return (MACH_RCV_PORT_DIED);

			if (mp->mp_count == 0)
				return (MACH_RCV_TIMED_OUT);
		}
	}

	/*
	 * Dequeue the message.
	 * XXX Do we really need to lock here? There could be
	 * only one reader process, so mm will not disapear
	 * except if there is a port refcount error in our code.
	 */
	rw_rlock(&mp->mp_msglock);
	mm = TAILQ_FIRST(&mp->mp_msglist);
#ifdef DEBUG_MACH_MSG
	printf("pid %d: dequeue message on port %p (id %d)\n",
	    p->p_pid, mp, mm->mm_msg->msgh_id);
#endif

	ret = MACH_MSG_SUCCESS;
	if (mm->mm_size > recv_size) {
		struct mach_short_reply sr;

		ret = MACH_RCV_TOO_LARGE;
		/*
		 * If MACH_RCV_LARGE was not set, destroy the message.
		 */
		if ((option & MACH_RCV_LARGE) == 0) {
			free(mm->mm_msg, M_MACH);
			mach_message_put_shlocked(mm);
			goto unlock;
		}

		/*
		 * If MACH_RCV_TOO_LARGE is set, then return
		 * a message with just header and trailer. The
		 * size in the header should correspond to the
		 * whole message, so just copy the whole header.
		 */
		memcpy(&sr, mm->mm_msg, sizeof(mach_msg_header_t));
		mach_set_trailer(&sr, sizeof(sr));

		if ((error = copyout(&sr, urm, sizeof(sr))) != 0) {
			ret = MACH_RCV_INVALID_DATA;
			goto unlock;
		}

		/* Dump the Mach message */
		ktrmmsg((char *)&sr, sizeof(sr));
		goto unlock;
	}

	/*
	 * Get rights carried by the message if it is not a
	 * reply from the kernel.
	 * XXX mm->mm_td could contain stall data. Reference
	 * the thread's kernel port instead?
	 */
	if (mm->mm_td != NULL) {
		mach_port_t *mnp;
#ifdef DEBUG_MACH
		printf("mach_msg: non kernel-reply message\n");
#endif
		/*
		 * Turn local and remote port names into
		 * names in the local process namespace.
		 */
		bits = MACH_MSGH_LOCAL_BITS(mm->mm_msg->msgh_bits);
		mnp = &mm->mm_msg->msgh_local_port;
		mach_trade_rights(td, mm->mm_td, mnp, bits);

		bits = MACH_MSGH_REMOTE_BITS(mm->mm_msg->msgh_bits);
		mnp = &mm->mm_msg->msgh_remote_port;
		mach_trade_rights(td, mm->mm_td, mnp, bits);

		/*
		 * The same operation must be done to all
		 * port descriptors carried with the message.
		 */
		if ((mm->mm_msg->msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
		    ((ret = mach_trade_rights_complex(td, mm)) != 0))
			goto unlock;

		/*
		 * swap local and remote ports, and
		 * corresponding bits as well.
		 */
		bits = (bits & 0xffff0000) |
		    ((bits & 0xff00) >> 8) |
		    ((bits & 0x00ff) << 8);
		tmp = mm->mm_msg->msgh_remote_port;
		mm->mm_msg->msgh_remote_port =
		    mm->mm_msg->msgh_local_port;
		mm->mm_msg->msgh_local_port = tmp;
	}

	/*
	 * Copy the message to userland.
	 */
	if ((error = copyout(mm->mm_msg, urm, mm->mm_size)) != 0) {
		ret = MACH_RCV_INVALID_DATA;
		goto unlock;
	}

	/* Dump the Mach message */
	ktrmmsg((char *)mm->mm_msg, mm->mm_size);

	free(mm->mm_msg, M_MACH);
	mach_message_put_shlocked(mm); /* decrease mp_count */
unlock:
	rw_runlock(&mp->mp_msglock);

	return (ret);
}


int
sys_mach_msg_trap(struct thread *td, struct mach_msg_trap_args *uap)
{
	/* {
		syscallarg(mach_msg_header_t *) msg;
		syscallarg(mach_msg_option_t) option;
		syscallarg(mach_msg_size_t) send_size;
		syscallarg(mach_msg_size_t) rcv_size;
		syscallarg(mach_port_name_t) rcv_name;
		syscallarg(mach_msg_timeout_t) timeout;
		syscallarg(mach_port_name_t) notify;
	} */
	struct mach_msg_overwrite_trap_args cup;

	cup.msg = uap->msg;
	cup.option = uap->option;
	cup.send_size = uap->send_size;
	cup.rcv_size = uap->rcv_size;
	cup.rcv_name = uap->rcv_name;
	cup.timeout = uap->timeout;
	cup.notify = uap->notify;
	cup.rcv_msg = NULL;
	cup.scatter_list_size = 0;

	return (sys_mach_msg_overwrite_trap(td, &cup));
}

static inline  struct thread *
mach_get_target_task(struct thread *td, struct mach_port *mp)
{
	struct proc *tp;
	struct thread *ttd;

	switch (mp->mp_datatype) {
	case MACH_MP_PROC:
		tp = (struct proc *)mp->mp_data;
		ttd = TAILQ_FIRST(&tp->p_threads);
		KASSERT(ttd != NULL, ("no threads in proc"));
		break;

	case MACH_MP_LWP:
		ttd = (struct thread *)mp->mp_data;
		break;

	default:
		ttd = td;
		break;
	}

	return (ttd);
}

static inline void
mach_drop_rights(struct mach_right *mr, int bits)
{
	int rights;

	switch (bits) {
	case MACH_MSG_TYPE_MOVE_SEND:
		rights = MACH_PORT_TYPE_SEND;
		break;
	case MACH_MSG_TYPE_MOVE_SEND_ONCE:
		rights = MACH_PORT_TYPE_SEND_ONCE;
		break;
	case MACH_MSG_TYPE_MOVE_RECEIVE:
		/* Recv. right is lost when msg is received */
	case MACH_MSG_TYPE_MAKE_SEND:
	case MACH_MSG_TYPE_COPY_SEND:
	case MACH_MSG_TYPE_MAKE_SEND_ONCE:
	default:
		rights = 0;
		break;
	}

	if (rights != 0)
		mach_right_put(mr, rights);
}

/*
 * When a messages is transmitted from one process to another,
 * we need to make sure the port names are in the receiver process
 * namespace.
 */
static inline void
mach_trade_rights(struct thread *ltd, struct thread *rtd, mach_port_t *mnp, int bits)
	/* ltd:		 local lwp (receiver, current lwp) */
	/* rtd:		 remote lwp (sender) */
	/* mnp:	 pointer to the port name */
	/* bits:		 right bits */
{
	int lr;			/* local right type (to be added) */
	int rr;			/* remote right type */
	struct mach_right *lmr;	/* right in the local process */
	struct mach_right *rmr;	/* right in the remote process */

	switch (bits) {
	case MACH_MSG_TYPE_MAKE_SEND:
		rr = MACH_PORT_TYPE_RECEIVE;
		lr = MACH_PORT_TYPE_SEND;
		break;

	case MACH_MSG_TYPE_COPY_SEND:
	case MACH_MSG_TYPE_MOVE_SEND:
		rr = MACH_PORT_TYPE_SEND;
		lr = MACH_PORT_TYPE_SEND;
		break;

	case MACH_MSG_TYPE_MAKE_SEND_ONCE:
		rr = MACH_PORT_TYPE_RECEIVE;
		lr = MACH_PORT_TYPE_SEND_ONCE;
		break;

	case MACH_MSG_TYPE_MOVE_SEND_ONCE:
		rr = MACH_PORT_TYPE_SEND_ONCE;
		lr = MACH_PORT_TYPE_SEND_ONCE;
		break;

	case MACH_MSG_TYPE_MOVE_RECEIVE:
		rr = MACH_PORT_TYPE_RECEIVE;
		lr = MACH_PORT_TYPE_RECEIVE;
		break;

	default:
		rr = 0;
		lr = 0;
		break;
	}

	/* Get the right in the remote process (sender) */
	rmr = NULL;
	if (lr != 0)
		rmr = mach_right_check(*mnp, rtd, rr);

	/* Translate it into a right in the local process (receiver) */
	if (rmr != NULL) {
		lmr = mach_right_get(rmr->mr_port, ltd, rr, 0);
		*mnp = lmr->mr_name;
	} else {
		*mnp = 0;
	}
}

/*
 * Turn rights carried by complex messages into rights in
 * the local namespace. Returns a Mach messsage error
 * XXX Nothing is there yet to remove the rights from the
 * sender namespace, it should be done at send time and it
 * is not done yet.
 */
static inline int
mach_trade_rights_complex(struct thread *td, struct mach_message *mm)
{
	struct mach_complex_msg *mcm;
	unsigned int i, count;
	unsigned long begin, end;

	/*
	 * Sanity check the descriptor count.
	 * Note that all descriptor types
	 * have the same size, hence it is
	 * safe to not take the descriptor
	 * type into account here.
	 */
	mcm = (struct mach_complex_msg *)mm->mm_msg;
	count = mcm->mcm_body.msgh_descriptor_count;
	begin = (u_long)mcm;
	end = (u_long)&mcm->mcm_desc.gen[count];

	if ((end - begin) > mm->mm_size) {
#ifdef DEBUG_MACH
		printf("msg id %d: invalid count\n", mm->mm_msg->msgh_id);
#endif
		return (MACH_SEND_INVALID_DATA);
	}

	for (i = 0; i < count; i++) {
		switch (mcm->mcm_desc.gen[i].type) {
		case MACH_MSG_PORT_DESCRIPTOR:
			mach_trade_rights(td, mm->mm_td,
			    &mcm->mcm_desc.port[i].name,
			    mcm->mcm_desc.port[i].disposition);
			break;

		case MACH_MSG_OOL_PORTS_DESCRIPTOR: {	/* XXX untested */
			struct thread *rtd;		/* remote LWP */
			void *lumnp;		/* local user address */
			void *rumnp;		/* remote user address */
			int disp;		/* disposition*/
			size_t size;		/* data size */
			int mcount;		/* descriptor count */
			mach_port_t *kmnp;
			void *kaddr;
			int error;
			int j;

			rtd = mm->mm_td;
			disp = mcm->mcm_desc.ool_ports[i].disposition;
			rumnp = mcm->mcm_desc.ool_ports[i].address;
			mcount = mcm->mcm_desc.ool_ports[i].count;
			size = mcount * sizeof(*kmnp);
			kaddr = NULL;
			lumnp = NULL;

			/* This allocates kmnp */
			error = mach_ool_copyin(rtd, rumnp, &kaddr, size, 0);
			if (error != 0)
				return (MACH_SEND_INVALID_DATA);

			kmnp = (mach_port_t *)kaddr;
			for (j = 0; j < mcount; j++)
				mach_trade_rights(td, mm->mm_td, &kmnp[j], disp);

			/* This frees kmnp */
			if ((error = mach_ool_copyout(td, kmnp, &lumnp,
			    size, MACH_OOL_FREE|MACH_OOL_TRACE)) != 0)
				return (MACH_SEND_INVALID_DATA);

			mcm->mcm_desc.ool_ports[i].address = lumnp;
			break;
		}

		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
#ifdef DEBUG_MACH
			printf("MACH_MSG_OOL_VOLATILE_DESCRIPTOR\n");
#endif
			/* FALLTHROUGH */
		case MACH_MSG_OOL_DESCRIPTOR: {	/* XXX untested */
			struct thread *rtd;		/* remote LWP */
			void *ludata;		/* local user address */
			void *rudata;		/* remote user address */
			size_t size;		/* data size */
			void *kdata;
			int error;

			rtd = mm->mm_td;
			rudata = mcm->mcm_desc.ool[i].address;
			size = mcm->mcm_desc.ool[i].size;
			kdata = NULL;
			ludata = NULL;

			/*
			 * XXX This is inefficient for large chunk of OOL
			 * memory. Think about remapping COW when possible.
			 */

			/* This allocates kdata */
			error = mach_ool_copyin(rtd, rudata, &kdata, size, 0);
			if (error != 0)
				return (MACH_SEND_INVALID_DATA);

			/* This frees kdata */
			if ((error = mach_ool_copyout(td, kdata, &ludata,
			    size, MACH_OOL_FREE|MACH_OOL_TRACE)) != 0)
				return (MACH_SEND_INVALID_DATA);

			mcm->mcm_desc.ool_ports[i].address = ludata;
			break;
		}
		default:
#ifdef DEBUG_MACH
			printf("unknown descriptor type %d\n",
			    mcm->mcm_desc.gen[i].type);
#endif
			break;
		}
	}

	return (MACH_MSG_SUCCESS);
}


inline int
mach_ool_copyin(struct thread *td, const void *uaddr, void **kaddr, size_t size, int flags)
{
	int error;
	void *kbuf;
	struct proc *p = td->td_proc;

	/*
	 * Sanity check OOL size to avoid DoS on malloc: useless once
	 * we remap data instead of copying it. In the meantime,
	 * disabled since it makes some OOL transfer fail.
	 */
#if 0
	if (size > MACH_MAX_OOL_LEN)
		return (ENOMEM);
#endif

	if (*kaddr == NULL)
		kbuf = malloc(size, M_MACH, M_WAITOK);
	else
		kbuf = *kaddr;

	if ((error = copyin_proc(p, uaddr, kbuf, size)) != 0) {
		if (*kaddr == NULL)
			free(kbuf, M_MACH);
		return (error);
	}

	if (size > PAGE_SIZE)
		size = PAGE_SIZE;
	if ((flags & MACH_OOL_TRACE))
		ktrmool(kaddr, size, uaddr);

	*kaddr = kbuf;
	return (0);
}

inline int
mach_ool_copyout(struct thread *td, const void *kaddr, void **uaddr, size_t size, int flags)
{
	vm_offset_t ubuf;
	int error;
	vm_map_t map;
	struct proc *p = td->td_proc;

	/*
	 * Sanity check OOL size to avoid DoS on malloc: useless once
	 * we remap data instead of copying it. In the meantime,
	 * disabled since it makes some OOL transfer fail.
	 */
#if 0
	if (size > MACH_MAX_OOL_LEN) {
		error = ENOMEM;
		goto out;
	}
#endif
	map = &p->p_vmspace->vm_map;
	if (uaddr == NULL || (vm_offset_t)*uaddr < PAGE_SIZE)
		flags |= MACH_VM_FLAGS_ANYWHERE;
	else
		ubuf = (vm_offset_t)*uaddr;
	if ((error = mach_vm_allocate(map, &ubuf, size, flags)))
		goto out;
	if ((error = copyout_proc(p, kaddr, (void *)ubuf, size)) != 0)
		goto out;

	if (size > PAGE_SIZE)
		size = PAGE_SIZE;
	if ((flags & MACH_OOL_TRACE))
		ktrmool(kaddr, size, (void *)ubuf);

out:
	if (flags & MACH_OOL_FREE)
		free(__DECONST(void *, kaddr), M_MACH); /*XXXUNCONST*/

	if (error == 0)
		*uaddr = (void *)ubuf;
	return (error);
}


inline void
mach_set_trailer(void *msgh, size_t size)
{
	mach_msg_trailer_t *trailer;
	char *msg = (char *)msgh;

	trailer = (mach_msg_trailer_t *)&msg[size - sizeof(*trailer)];
	trailer->msgh_trailer_type = MACH_MSG_TRAILER_FORMAT_0;
	trailer->msgh_trailer_size = sizeof(*trailer);
}

inline void
mach_set_header(void *rep, void *req, size_t size)
{
	mach_msg_header_t *rephdr = rep;
	mach_msg_header_t *reqhdr = req;

	rephdr->msgh_bits =
		MACH_MSGH_REPLY_LOCAL_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE);
	rephdr->msgh_size = size - sizeof(mach_msg_trailer_t);
	rephdr->msgh_local_port = reqhdr->msgh_local_port;
	rephdr->msgh_remote_port = 0;
	rephdr->msgh_id = reqhdr->msgh_id + 100;
}

inline void
mach_add_port_desc(void *msg, mach_port_name_t name)
{
	struct mach_complex_msg *mcm = msg;
	int i;

	if ((mcm->mcm_header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0) {
		mcm->mcm_header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
		mcm->mcm_body.msgh_descriptor_count = 0;
	}

	i = mcm->mcm_body.msgh_descriptor_count;

	mcm->mcm_desc.port[i].name = name;
	mcm->mcm_desc.port[i].disposition = MACH_MSG_TYPE_MOVE_SEND;
	mcm->mcm_desc.port[i].type = MACH_MSG_PORT_DESCRIPTOR;

	mcm->mcm_body.msgh_descriptor_count++;
}

inline void
mach_add_ool_ports_desc(void *msg, void *addr, int count)
{
	struct mach_complex_msg *mcm = msg;
	int i;

	if ((mcm->mcm_header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0) {
		mcm->mcm_header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
		mcm->mcm_body.msgh_descriptor_count = 0;
	}

	i = mcm->mcm_body.msgh_descriptor_count;

	mcm->mcm_desc.ool_ports[i].address = addr;
	mcm->mcm_desc.ool_ports[i].count = count;
	mcm->mcm_desc.ool_ports[i].copy = MACH_MSG_ALLOCATE;
	mcm->mcm_desc.ool_ports[i].disposition = MACH_MSG_TYPE_MOVE_SEND;
	mcm->mcm_desc.ool_ports[i].type = MACH_MSG_OOL_PORTS_DESCRIPTOR;

	mcm->mcm_body.msgh_descriptor_count++;
}

inline void mach_add_ool_desc(msg, addr, size)
	void *msg;
	void *addr;
	size_t size;
{
	struct mach_complex_msg *mcm = msg;
	int i;

	if ((mcm->mcm_header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0) {
		mcm->mcm_header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
		mcm->mcm_body.msgh_descriptor_count = 0;
	}

	i = mcm->mcm_body.msgh_descriptor_count;

	mcm->mcm_desc.ool[i].address = addr;
	mcm->mcm_desc.ool[i].size = size;
	mcm->mcm_desc.ool[i].deallocate = 0;
	mcm->mcm_desc.ool[i].copy = MACH_MSG_ALLOCATE;
	mcm->mcm_desc.ool[i].type = MACH_MSG_OOL_DESCRIPTOR;

	mcm->mcm_body.msgh_descriptor_count++;
}

void
mach_message_init(void)
{

	mach_message_zone =
		uma_zcreate("mach_message_zone", sizeof (struct mach_message),
					NULL, NULL, NULL, NULL, 0/* align*/, 0/*flags*/);
}

struct mach_message *
mach_message_get(mach_msg_header_t *msgh, size_t size, struct mach_port *mp, struct thread *td)
{
	struct mach_message *mm;

	mm = uma_zalloc(mach_message_zone, M_WAITOK);
	memset(mm, 0, sizeof(*mm));
	mm->mm_msg = msgh;
	mm->mm_size = size;
	mm->mm_port = mp;
	mm->mm_td = td;

	rw_wlock(&mp->mp_msglock);
	TAILQ_INSERT_TAIL(&mp->mp_msglist, mm, mm_list);
	mp->mp_count++;
	rw_wunlock(&mp->mp_msglock);

	return (mm);
}

void
mach_message_put(struct mach_message *mm)
{
	struct mach_port *mp;

	mp = mm->mm_port;

	rw_wlock(&mp->mp_msglock);
	mach_message_put_exclocked(mm);
	rw_wunlock(&mp->mp_msglock);
}

void
mach_message_put_shlocked(struct mach_message *mm)
{
	struct mach_port *mp;

	mp = mm->mm_port;

	if (!rw_try_upgrade(&mp->mp_msglock)) {
		/* XXX  */
		rw_runlock(&mp->mp_msglock);
		rw_wlock(&mp->mp_msglock);
	}
	mach_message_put_exclocked(mm);
	rw_downgrade(&mp->mp_msglock);
}

void
mach_message_put_exclocked(struct mach_message *mm)
{
	struct mach_port *mp;

	mp = mm->mm_port;

	TAILQ_REMOVE(&mp->mp_msglist, mm, mm_list);
	mp->mp_count--;

	uma_zfree(mach_message_zone, mm);
}


#ifdef DEBUG_MACH
void
mach_debug_message(void)
{
#if 0
	struct thread *td;
	struct mach_emuldata *med;
	struct mach_right *mr;
	struct mach_right *mrs;
	struct mach_port *mp;
	struct mach_message *mm;

	LIST_FOREACH(l, &alllwp, l_list) {
		if ((td->td_proc->p_emul != &emul_mach) &&
#ifdef COMPAT_DARWIN
		    (td->td_proc->p_emul != &emul_darwin) &&
#endif
		    1)
			continue;

		med = td->td_proc->p_emuldata;
		LIST_FOREACH(mr, &med->med_right, mr_list)
			if ((mr->mr_type & MACH_PORT_TYPE_PORT_SET) == 0) {
				mp = mr->mr_port;
				if (mp == NULL)
					continue;

				printf("port %p(%d) ", mp, mp->mp_count);

				TAILQ_FOREACH(mm, &mp->mp_msglist, mm_list)
					printf("%d ", mm->mm_msg->msgh_id);

				printf("\n");
				continue;
			}
			/* Port set... */
			LIST_FOREACH(mrs, &mr->mr_set, mr_setlist) {
				mp = mrs->mr_port;
				if (mp == NULL)
					continue;

				printf("port %p(%d) ", mp, mp->mp_count);

				TAILQ_FOREACH(mm, &mp->mp_msglist, mm_list)
					printf("%d ", mm->mm_msg->msgh_id);

				printf("\n");
			}
	}
#endif
}

#endif /* DEBUG_MACH */

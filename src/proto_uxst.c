/*
 * UNIX SOCK_STREAM protocol layer (uxst)
 *
 * Copyright 2000-2010 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <haproxy/api.h>
#include <haproxy/connection.h>
#include <haproxy/errors.h>
#include <haproxy/fd.h>
#include <haproxy/global.h>
#include <haproxy/list.h>
#include <haproxy/listener.h>
#include <haproxy/log.h>
#include <haproxy/protocol.h>
#include <haproxy/sock.h>
#include <haproxy/sock_unix.h>
#include <haproxy/time.h>
#include <haproxy/tools.h>
#include <haproxy/version.h>


static int uxst_bind_listener(struct listener *listener, char *errmsg, int errlen);
static int uxst_bind_listeners(struct protocol *proto, char *errmsg, int errlen);
static int uxst_unbind_listeners(struct protocol *proto);
static int uxst_connect_server(struct connection *conn, int flags);
static void uxst_add_listener(struct listener *listener, int port);
static int uxst_pause_listener(struct listener *l);

/* Note: must not be declared <const> as its list will be overwritten */
static struct protocol proto_unix = {
	.name = "unix_stream",
	.sock_domain = PF_UNIX,
	.sock_type = SOCK_STREAM,
	.sock_prot = 0,
	.sock_family = AF_UNIX,
	.sock_addrlen = sizeof(struct sockaddr_un),
	.l3_addrlen = sizeof(((struct sockaddr_un*)0)->sun_path),/* path len */
	.accept = &listener_accept,
	.connect = &uxst_connect_server,
	.bind = uxst_bind_listener,
	.bind_all = uxst_bind_listeners,
	.unbind_all = uxst_unbind_listeners,
	.enable_all = enable_all_listeners,
	.disable_all = disable_all_listeners,
	.get_src = sock_get_src,
	.get_dst = sock_get_dst,
	.pause = uxst_pause_listener,
	.add = uxst_add_listener,
	.addrcmp = sock_unix_addrcmp,
	.listeners = LIST_HEAD_INIT(proto_unix.listeners),
	.nb_listeners = 0,
};

INITCALL1(STG_REGISTER, protocol_register, &proto_unix);

/********************************
 * 1) low-level socket functions
 ********************************/


/********************************
 * 2) listener-oriented functions
 ********************************/

/* This function creates a UNIX socket associated to the listener. It changes
 * the state from ASSIGNED to LISTEN. The socket is NOT enabled for polling.
 * The return value is composed from ERR_NONE, ERR_RETRYABLE and ERR_FATAL. It
 * may return a warning or an error message in <errmsg> if the message is at
 * most <errlen> bytes long (including '\0'). Note that <errmsg> may be NULL if
 * <errlen> is also zero.
 */
static int uxst_bind_listener(struct listener *listener, char *errmsg, int errlen)
{
	int fd;
	char tempname[MAXPATHLEN];
	char backname[MAXPATHLEN];
	struct sockaddr_un addr;
	const char *msg = NULL;
	const char *path;
	int maxpathlen;
	int ext, ready;
	socklen_t ready_len;
	int err;
	int ret;

	err = ERR_NONE;

	/* ensure we never return garbage */
	if (errlen)
		*errmsg = 0;

	if (listener->state != LI_ASSIGNED)
		return ERR_NONE; /* already bound */
		
	if (listener->fd == -1)
		listener->fd = sock_find_compatible_fd(listener);

	path = ((struct sockaddr_un *)&listener->addr)->sun_path;

	maxpathlen = MIN(MAXPATHLEN, sizeof(addr.sun_path));

	/* if the listener already has an fd assigned, then we were offered the
	 * fd by an external process (most likely the parent), and we don't want
	 * to create a new socket. However we still want to set a few flags on
	 * the socket.
	 */
	fd = listener->fd;
	ext = (fd >= 0);
	if (ext)
		goto fd_ready;

	if (path[0]) {
		ret = snprintf(tempname, maxpathlen, "%s.%d.tmp", path, pid);
		if (ret < 0 || ret >= sizeof(addr.sun_path)) {
			err |= ERR_FATAL | ERR_ALERT;
			msg = "name too long for UNIX socket (limit usually 97)";
			goto err_return;
		}

		ret = snprintf(backname, maxpathlen, "%s.%d.bak", path, pid);
		if (ret < 0 || ret >= maxpathlen) {
			err |= ERR_FATAL | ERR_ALERT;
			msg = "name too long for UNIX socket (limit usually 97)";
			goto err_return;
		}

		/* 2. clean existing orphaned entries */
		if (unlink(tempname) < 0 && errno != ENOENT) {
			err |= ERR_FATAL | ERR_ALERT;
			msg = "error when trying to unlink previous UNIX socket";
			goto err_return;
		}

		if (unlink(backname) < 0 && errno != ENOENT) {
			err |= ERR_FATAL | ERR_ALERT;
			msg = "error when trying to unlink previous UNIX socket";
			goto err_return;
		}

		/* 3. backup existing socket */
		if (link(path, backname) < 0 && errno != ENOENT) {
			err |= ERR_FATAL | ERR_ALERT;
			msg = "error when trying to preserve previous UNIX socket";
			goto err_return;
		}

		/* Note: this test is redundant with the snprintf one above and
		 * will never trigger, it's just added as the only way to shut
		 * gcc's painfully dumb warning about possibly truncated output
		 * during strncpy(). Don't move it above or smart gcc will not
		 * see it!
		 */
		if (strlen(tempname) >= sizeof(addr.sun_path)) {
			err |= ERR_FATAL | ERR_ALERT;
			msg = "name too long for UNIX socket (limit usually 97)";
			goto err_return;
		}

		strncpy(addr.sun_path, tempname, sizeof(addr.sun_path) - 1);
		addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
	}
	else {
		/* first char is zero, it's an abstract socket whose address
		 * is defined by all the bytes past this zero.
		 */
		memcpy(addr.sun_path, path, sizeof(addr.sun_path));
	}
	addr.sun_family = AF_UNIX;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "cannot create UNIX socket";
		goto err_unlink_back;
	}

 fd_ready:
	if (fd >= global.maxsock) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "socket(): not enough free sockets, raise -n argument";
		goto err_unlink_temp;
	}
	
	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "cannot make UNIX socket non-blocking";
		goto err_unlink_temp;
	}
	
	if (!ext && bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		/* note that bind() creates the socket <tempname> on the file system */
		if (errno == EADDRINUSE) {
			/* the old process might still own it, let's retry */
			err |= ERR_RETRYABLE | ERR_ALERT;
			msg = "cannot listen to socket";
		}
		else {
			err |= ERR_FATAL | ERR_ALERT;
			msg = "cannot bind UNIX socket";
		}
		goto err_unlink_temp;
	}

	/* <uid> and <gid> different of -1 will be used to change the socket owner.
	 * If <mode> is not 0, it will be used to restrict access to the socket.
	 * While it is known not to be portable on every OS, it's still useful
	 * where it works. We also don't change permissions on abstract sockets.
	 */
	if (!ext && path[0] &&
	    (((listener->bind_conf->ux.uid != -1 || listener->bind_conf->ux.gid != -1) &&
	      (chown(tempname, listener->bind_conf->ux.uid, listener->bind_conf->ux.gid) == -1)) ||
	     (listener->bind_conf->ux.mode != 0 && chmod(tempname, listener->bind_conf->ux.mode) == -1))) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "cannot change UNIX socket ownership";
		goto err_unlink_temp;
	}

	ready = 0;
	ready_len = sizeof(ready);
	if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &ready, &ready_len) == -1)
		ready = 0;

	if (!(ext && ready) && /* only listen if not already done by external process */
	    listen(fd, listener_backlog(listener)) < 0) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "cannot listen to UNIX socket";
		goto err_unlink_temp;
	}

	/* Point of no return: we are ready, we'll switch the sockets. We don't
	 * fear losing the socket <path> because we have a copy of it in
	 * backname. Abstract sockets are not renamed.
	 */
	if (!ext && path[0] && rename(tempname, path) < 0) {
		err |= ERR_FATAL | ERR_ALERT;
		msg = "cannot switch final and temporary UNIX sockets";
		goto err_rename;
	}

	/* Cleanup: only unlink if we didn't inherit the fd from the parent */
	if (!ext && path[0])
		unlink(backname);

	/* the socket is now listening */
	listener->fd = fd;
	listener->state = LI_LISTEN;

	fd_insert(fd, listener, listener->proto->accept,
	          thread_mask(listener->bind_conf->bind_thread) & all_threads_mask);

	/* for now, all regularly bound UNIX listeners are exportable */
	if (!(listener->options & LI_O_INHERITED))
		fdtab[fd].exported = 1;

	return err;

 err_rename:
	ret = rename(backname, path);
	if (ret < 0 && errno == ENOENT)
		unlink(path);
 err_unlink_temp:
	if (!ext && path[0])
		unlink(tempname);
	close(fd);
 err_unlink_back:
	if (!ext && path[0])
		unlink(backname);
 err_return:
	if (msg && errlen) {
		if (!ext)
			snprintf(errmsg, errlen, "%s [%s]", msg, path);
		else
			snprintf(errmsg, errlen, "%s [fd %d]", msg, fd);
	}
	return err;
}

/* This function closes the UNIX sockets for the specified listener.
 * The listener enters the LI_ASSIGNED state. It always returns ERR_NONE.
 */
static int uxst_unbind_listener(struct listener *listener)
{
	if (listener->state > LI_ASSIGNED) {
		unbind_listener(listener);
	}
	return ERR_NONE;
}

/* Add <listener> to the list of unix stream listeners (port is ignored). The
 * listener's state is automatically updated from LI_INIT to LI_ASSIGNED.
 * The number of listeners for the protocol is updated.
 *
 * Must be called with proto_lock held.
 *
 */
static void uxst_add_listener(struct listener *listener, int port)
{
	if (listener->state != LI_INIT)
		return;
	listener->state = LI_ASSIGNED;
	listener->proto = &proto_unix;
	LIST_ADDQ(&proto_unix.listeners, &listener->proto_list);
	proto_unix.nb_listeners++;
}

/* Pause a listener. Returns < 0 in case of failure, 0 if the listener
 * was totally stopped, or > 0 if correctly paused. Nothing is done for
 * plain unix sockets since currently it's the new process which handles
 * the renaming. Abstract sockets are completely unbound.
 */
static int uxst_pause_listener(struct listener *l)
{
	if (((struct sockaddr_un *)&l->addr)->sun_path[0])
		return 1;

	/* Listener's lock already held. Call lockless version of
	 * unbind_listener. */
	do_unbind_listener(l, 1);
	return 0;
}


/*
 * This function initiates a UNIX connection establishment to the target assigned
 * to connection <conn> using (si->{target,dst}). The source address is ignored
 * and will be selected by the system. conn->target may point either to a valid
 * server or to a backend, depending on conn->target. Only OBJ_TYPE_PROXY and
 * OBJ_TYPE_SERVER are supported. The <data> parameter is a boolean indicating
 * whether there are data waiting for being sent or not, in order to adjust data
 * write polling and on some platforms. The <delack> argument is ignored.
 *
 * Note that a pending send_proxy message accounts for data.
 *
 * It can return one of :
 *  - SF_ERR_NONE if everything's OK
 *  - SF_ERR_SRVTO if there are no more servers
 *  - SF_ERR_SRVCL if the connection was refused by the server
 *  - SF_ERR_PRXCOND if the connection has been limited by the proxy (maxconn)
 *  - SF_ERR_RESOURCE if a system resource is lacking (eg: fd limits, ports, ...)
 *  - SF_ERR_INTERNAL for any other purely internal errors
 * Additionally, in the case of SF_ERR_RESOURCE, an emergency log will be emitted.
 *
 * The connection's fd is inserted only when SF_ERR_NONE is returned, otherwise
 * it's invalid and the caller has nothing to do.
 */
static int uxst_connect_server(struct connection *conn, int flags)
{
	int fd;
	struct server *srv;
	struct proxy *be;

	switch (obj_type(conn->target)) {
	case OBJ_TYPE_PROXY:
		be = objt_proxy(conn->target);
		srv = NULL;
		break;
	case OBJ_TYPE_SERVER:
		srv = objt_server(conn->target);
		be = srv->proxy;
		break;
	default:
		conn->flags |= CO_FL_ERROR;
		return SF_ERR_INTERNAL;
	}

	if ((fd = conn->handle.fd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1) {
		qfprintf(stderr, "Cannot get a server socket.\n");

		if (errno == ENFILE) {
			conn->err_code = CO_ER_SYS_FDLIM;
			send_log(be, LOG_EMERG,
				 "Proxy %s reached system FD limit (maxsock=%d). Please check system tunables.\n",
				 be->id, global.maxsock);
		}
		else if (errno == EMFILE) {
			conn->err_code = CO_ER_PROC_FDLIM;
			send_log(be, LOG_EMERG,
				 "Proxy %s reached process FD limit (maxsock=%d). Please check 'ulimit-n' and restart.\n",
				 be->id, global.maxsock);
		}
		else if (errno == ENOBUFS || errno == ENOMEM) {
			conn->err_code = CO_ER_SYS_MEMLIM;
			send_log(be, LOG_EMERG,
				 "Proxy %s reached system memory limit (maxsock=%d). Please check system tunables.\n",
				 be->id, global.maxsock);
		}
		else if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) {
			conn->err_code = CO_ER_NOPROTO;
		}
		else
			conn->err_code = CO_ER_SOCK_ERR;

		/* this is a resource error */
		conn->flags |= CO_FL_ERROR;
		return SF_ERR_RESOURCE;
	}

	if (fd >= global.maxsock) {
		/* do not log anything there, it's a normal condition when this option
		 * is used to serialize connections to a server !
		 */
		ha_alert("socket(): not enough free sockets. Raise -n argument. Giving up.\n");
		close(fd);
		conn->err_code = CO_ER_CONF_FDLIM;
		conn->flags |= CO_FL_ERROR;
		return SF_ERR_PRXCOND; /* it is a configuration limit */
	}

	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
		qfprintf(stderr,"Cannot set client socket to non blocking mode.\n");
		close(fd);
		conn->err_code = CO_ER_SOCK_ERR;
		conn->flags |= CO_FL_ERROR;
		return SF_ERR_INTERNAL;
	}

	if (master == 1 && (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)) {
		ha_alert("Cannot set CLOEXEC on client socket.\n");
		close(fd);
		conn->err_code = CO_ER_SOCK_ERR;
		conn->flags |= CO_FL_ERROR;
		return SF_ERR_INTERNAL;
	}

	/* if a send_proxy is there, there are data */
	if (conn->send_proxy_ofs)
		flags |= CONNECT_HAS_DATA;

	if (global.tune.server_sndbuf)
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &global.tune.server_sndbuf, sizeof(global.tune.server_sndbuf));

	if (global.tune.server_rcvbuf)
                setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &global.tune.server_rcvbuf, sizeof(global.tune.server_rcvbuf));

	if (connect(fd, (struct sockaddr *)conn->dst, get_addr_len(conn->dst)) == -1) {
		if (errno == EINPROGRESS || errno == EALREADY) {
			conn->flags |= CO_FL_WAIT_L4_CONN;
		}
		else if (errno == EISCONN) {
			conn->flags &= ~CO_FL_WAIT_L4_CONN;
		}
		else if (errno == EAGAIN || errno == EADDRINUSE || errno == EADDRNOTAVAIL) {
			char *msg;
			if (errno == EAGAIN || errno == EADDRNOTAVAIL) {
				msg = "can't connect to destination unix socket, check backlog size on the server";
				conn->err_code = CO_ER_FREE_PORTS;
			}
			else {
				msg = "local address already in use";
				conn->err_code = CO_ER_ADDR_INUSE;
			}

			qfprintf(stderr,"Connect() failed for backend %s: %s.\n", be->id, msg);
			close(fd);
			send_log(be, LOG_ERR, "Connect() failed for backend %s: %s.\n", be->id, msg);
			conn->flags |= CO_FL_ERROR;
			return SF_ERR_RESOURCE;
		}
		else if (errno == ETIMEDOUT) {
			close(fd);
			conn->err_code = CO_ER_SOCK_ERR;
			conn->flags |= CO_FL_ERROR;
			return SF_ERR_SRVTO;
		}
		else {	// (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EACCES || errno == EPERM)
			close(fd);
			conn->err_code = CO_ER_SOCK_ERR;
			conn->flags |= CO_FL_ERROR;
			return SF_ERR_SRVCL;
		}
	}
	else {
		/* connect() already succeeded, which is quite usual for unix
		 * sockets. Let's avoid a second connect() probe to complete it.
		 */
		conn->flags &= ~CO_FL_WAIT_L4_CONN;
	}

	conn->flags |= CO_FL_ADDR_TO_SET;

	/* Prepare to send a few handshakes related to the on-wire protocol. */
	if (conn->send_proxy_ofs)
		conn->flags |= CO_FL_SEND_PROXY;

	conn_ctrl_init(conn);       /* registers the FD */
	fdtab[fd].linger_risk = 0;  /* no need to disable lingering */

	if (conn->flags & CO_FL_WAIT_L4_CONN) {
		fd_want_send(fd);
		fd_cant_send(fd);
		fd_cant_recv(fd);
	}

	if (conn_xprt_init(conn) < 0) {
		conn_full_close(conn);
		conn->flags |= CO_FL_ERROR;
		return SF_ERR_RESOURCE;
	}

	return SF_ERR_NONE;  /* connection is OK */
}


/********************************
 * 3) protocol-oriented functions
 ********************************/


/* This function creates all UNIX sockets bound to the protocol entry <proto>.
 * It is intended to be used as the protocol's bind_all() function.
 * The sockets will be registered but not added to any fd_set, in order not to
 * loose them across the fork(). A call to uxst_enable_listeners() is needed
 * to complete initialization.
 *
 * Must be called with proto_lock held.
 *
 * The return value is composed from ERR_NONE, ERR_RETRYABLE and ERR_FATAL.
 */
static int uxst_bind_listeners(struct protocol *proto, char *errmsg, int errlen)
{
	struct listener *listener;
	int err = ERR_NONE;

	list_for_each_entry(listener, &proto->listeners, proto_list) {
		err |= uxst_bind_listener(listener, errmsg, errlen);
		if (err & ERR_ABORT)
			break;
	}
	return err;
}


/* This function stops all listening UNIX sockets bound to the protocol
 * <proto>. It does not detaches them from the protocol.
 * It always returns ERR_NONE.
 *
 * Must be called with proto_lock held.
 *
 */
static int uxst_unbind_listeners(struct protocol *proto)
{
	struct listener *listener;

	list_for_each_entry(listener, &proto->listeners, proto_list)
		uxst_unbind_listener(listener);
	return ERR_NONE;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */

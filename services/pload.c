/*
 * Copyright (c) 2008-2009 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

#include <corosync/swab.h>
#include <corosync/corotypes.h>
#include <corosync/ipc_gen.h>
#include <corosync/lcr/lcr_comp.h>
#include <corosync/engine/coroapi.h>
#include <corosync/ipc_pload.h>
#include <corosync/list.h>
#include <corosync/engine/logsys.h>

LOGSYS_DECLARE_SUBSYS ("PLOAD", LOG_INFO);

enum pload_exec_message_req_types {
	MESSAGE_REQ_EXEC_PLOAD_START = 0,
	MESSAGE_REQ_EXEC_PLOAD_MCAST = 1
};

/*
 * Service Interfaces required by service_message_handler struct
 */
static int pload_exec_init_fn (
	struct corosync_api_v1 *corosync_api);

static void pload_confchg_fn (
	enum totem_configuration_type configuration_type,
	unsigned int *member_list, int member_list_entries,
	unsigned int *left_list, int left_list_entries,
	unsigned int *joined_list, int joined_list_entries,
	struct memb_ring_id *ring_id);

static void message_handler_req_exec_pload_start (const void *msg,
						  unsigned int nodeid);

static void message_handler_req_exec_pload_mcast (const void *msg,
						  unsigned int nodeid);

static void req_exec_pload_start_endian_convert (void *msg);

static void req_exec_pload_mcast_endian_convert (void *msg);

static void message_handler_req_pload_start (void *conn, void *msg);

static int pload_lib_init_fn (void *conn);

static int pload_lib_exit_fn (void *conn);

static char buffer[1000000];

static unsigned int msgs_delivered = 0;

static unsigned int msgs_wanted = 0;

static unsigned int msg_size = 0;

static unsigned int msg_code = 1;

static unsigned int msgs_sent = 0;

	
static struct corosync_api_v1 *api;

struct req_exec_pload_start {
	mar_req_header_t header;
	unsigned int msg_code;
	unsigned int msg_count;
	unsigned int msg_size;
	unsigned int time_interval;
};

struct req_exec_pload_mcast {
	mar_req_header_t header;
	unsigned int msg_code;
};

static struct corosync_lib_handler pload_lib_engine[] =
{
	{ /* 0 */
		.lib_handler_fn		= message_handler_req_pload_start,
		.response_size		= sizeof (struct res_lib_pload_start),
		.response_id		= MESSAGE_RES_PLOAD_START,
		.flow_control		= CS_LIB_FLOW_CONTROL_NOT_REQUIRED
	}
};

static struct corosync_exec_handler pload_exec_engine[] =
{
	{
		.exec_handler_fn 	= message_handler_req_exec_pload_start,
		.exec_endian_convert_fn	= req_exec_pload_start_endian_convert
	},
	{
		.exec_handler_fn 	= message_handler_req_exec_pload_mcast,
		.exec_endian_convert_fn	= req_exec_pload_mcast_endian_convert
	}
};

struct corosync_service_engine pload_service_engine = {
	.name			= "corosync profile loading service",
	.id			= PLOAD_SERVICE,
	.private_data_size	= 0,
	.flow_control		= CS_LIB_FLOW_CONTROL_REQUIRED, 
	.lib_init_fn		= pload_lib_init_fn,
	.lib_exit_fn		= pload_lib_exit_fn,
	.lib_engine		= pload_lib_engine,
	.lib_engine_count	= sizeof (pload_lib_engine) / sizeof (struct corosync_lib_handler),
	.exec_engine		= pload_exec_engine,
	.exec_engine_count	= sizeof (pload_exec_engine) / sizeof (struct corosync_exec_handler),
	.confchg_fn		= pload_confchg_fn,
	.exec_init_fn		= pload_exec_init_fn,
	.exec_dump_fn		= NULL
};

static DECLARE_LIST_INIT (confchg_notify);

/*
 * Dynamic loading descriptor
 */

static struct corosync_service_engine *pload_get_service_engine_ver0 (void);

static struct corosync_service_engine_iface_ver0 pload_service_engine_iface = {
	.corosync_get_service_engine_ver0	= pload_get_service_engine_ver0
};

static struct lcr_iface corosync_pload_ver0[1] = {
	{
		.name			= "corosync_pload",
		.version		= 0,
		.versions_replace	= 0,
		.versions_replace_count = 0,
		.dependencies		= 0,
		.dependency_count	= 0,
		.constructor		= NULL,
		.destructor		= NULL,
		.interfaces		= NULL,
	}
};

static struct lcr_comp pload_comp_ver0 = {
	.iface_count	= 1,
	.ifaces		= corosync_pload_ver0
};

static struct corosync_service_engine *pload_get_service_engine_ver0 (void)
{
	return (&pload_service_engine);
}

__attribute__ ((constructor)) static void pload_comp_register (void) {
	lcr_interfaces_set (&corosync_pload_ver0[0], &pload_service_engine_iface);

	lcr_component_register (&pload_comp_ver0);
}

static int pload_exec_init_fn (
	struct corosync_api_v1 *corosync_api)
{
	api = corosync_api;

	return 0;
}

static void pload_confchg_fn (
	enum totem_configuration_type configuration_type,
	unsigned int *member_list, int member_list_entries,
	unsigned int *left_list, int left_list_entries,
	unsigned int *joined_list, int joined_list_entries,
	struct memb_ring_id *ring_id)
{
}

static int pload_lib_init_fn (void *conn)
{
	return (0);
}

static int pload_lib_exit_fn (void *conn)
{
	return (0);
}

static void message_handler_req_pload_start (void *conn, void *msg)
{
	struct req_lib_pload_start *req_lib_pload_start = (struct req_lib_pload_start *)msg;
	struct req_exec_pload_start req_exec_pload_start;
	struct iovec iov;

	req_exec_pload_start.header.id =
		SERVICE_ID_MAKE (PLOAD_SERVICE, MESSAGE_REQ_EXEC_PLOAD_START);
	req_exec_pload_start.msg_code = req_lib_pload_start->msg_code;
	req_exec_pload_start.msg_size = req_lib_pload_start->msg_size;
	req_exec_pload_start.msg_count = req_lib_pload_start->msg_count;
	req_exec_pload_start.time_interval = req_lib_pload_start->time_interval;
	iov.iov_base = &req_exec_pload_start;
	iov.iov_len = sizeof (struct req_exec_pload_start);

	api->totem_mcast (&iov, 1, TOTEM_AGREED);
}

static void req_exec_pload_start_endian_convert (void *msg)
{
}

static void req_exec_pload_mcast_endian_convert (void *msg)
{
}

static int send_message (enum totem_callback_token_type type, void *arg)
{
	struct req_exec_pload_mcast req_exec_pload_mcast;
	struct iovec iov[2];
	unsigned int res;
	int iov_len = 2;

	req_exec_pload_mcast.header.id =
		SERVICE_ID_MAKE (PLOAD_SERVICE, MESSAGE_REQ_EXEC_PLOAD_MCAST);
	req_exec_pload_mcast.header.size = sizeof (struct req_exec_pload_mcast) + msg_size;

	iov[0].iov_base = &req_exec_pload_mcast;
	iov[0].iov_len = sizeof (struct req_exec_pload_mcast);
	iov[1].iov_base = buffer;
	iov[1].iov_len = msg_size - sizeof (struct req_exec_pload_mcast);
	if (iov[1].iov_len < 0) {
		iov_len = 1;
	}

	do {
		res = api->totem_mcast (iov, iov_len, TOTEM_AGREED);
		if (res == -1) {
			break;
		} else {
			msgs_sent++;
			msg_code++;
		}
	} while (msgs_sent <= msgs_wanted);
	if (msgs_sent == msgs_wanted) {
		return (0);
	} else {
		return (-1);
	}
}

static void *token_callback;
static void start_mcasting (void)
{
	api->totem_callback_token_create (
		&token_callback,
		TOTEM_CALLBACK_TOKEN_RECEIVED,
		1,
		send_message,	
		&token_callback);
} 

static void message_handler_req_exec_pload_start (
	const void *msg,
	unsigned int nodeid)
{
	const struct req_exec_pload_start *req_exec_pload_start = msg;

	msgs_wanted = req_exec_pload_start->msg_count;
	msg_size = req_exec_pload_start->msg_size;
	msg_code = req_exec_pload_start->msg_code;

	start_mcasting ();
}

# define timersub(a, b, result)                                               \
  do {                                                                        \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;                             \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;                          \
    if ((result)->tv_usec < 0) {                                              \
      --(result)->tv_sec;                                                     \
      (result)->tv_usec += 1000000;                                           \
    }                                                                         \
  } while (0)

struct timeval tv1;
struct timeval tv2;
struct timeval tv_elapsed;
int last_msg_no = 0;

static void message_handler_req_exec_pload_mcast (
	const void *msg,
	unsigned int nodeid)
{
	const struct req_exec_pload_mcast *pload_mcast = msg;

	assert (pload_mcast->msg_code - 1 == last_msg_no);
	last_msg_no = pload_mcast->msg_code;
	if (msgs_delivered == 0) {
		gettimeofday (&tv1, NULL);
	}
	msgs_delivered += 1;
	if (msgs_delivered == msgs_wanted) {
		gettimeofday (&tv2, NULL);
		timersub (&tv2, &tv1, &tv_elapsed);
	        printf ("%5d Writes ", msgs_delivered);
		printf ("%5d bytes per write ", msg_size);
		printf ("%7.3f Seconds runtime ",
		(tv_elapsed.tv_sec + (tv_elapsed.tv_usec / 1000000.0)));
		printf ("%9.3f TP/s ",
		((float)msgs_delivered) /  (tv_elapsed.tv_sec + (tv_elapsed.tv_usec / 1000000.0)));
		printf ("%7.3f MB/s.\n",
		((float)msgs_delivered) * ((float)msg_size) /  ((tv_elapsed.tv_sec + (tv_elapsed.tv_usec / 1000000.0)) * 1000000.0));
	}
}

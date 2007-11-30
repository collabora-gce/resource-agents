/*
  Copyright Red Hat, Inc. 2007

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License version 2 as published
  by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
  MA 02139, USA.
*/
#include <assert.h>
#include <platform.h>
#include <message.h>
#include <members.h>
#include <stdio.h>
#include <string.h>
#include <resgroup.h>
#include <clulog.h>
#include <lock.h>
#include <rg_locks.h>
#include <ccs.h>
#include <rg_queue.h>
#include <msgsimple.h>
#include <res-ocf.h>
#include <event.h>


/*
 * Send a message to the target node to start the service.
 */
int svc_start_remote(char *svcName, int request, uint32_t target);
void svc_report_failure(char *);
int get_service_state_internal(char *svcName, rg_state_t *svcStatus);


/**
 *
 */
int
service_op_start(char *svcName,
		 int *target_list,
		 int target_list_len,
		 int *new_owner)
{
	int target;
	int ret, x;
	rg_state_t svcStatus;
	
	if (get_service_state_internal(svcName, &svcStatus) < 0) {
		return RG_EFAIL;
	}

	if (svcStatus.rs_state == RG_STATE_FAILED ||
	    svcStatus.rs_state == RG_STATE_UNINITIALIZED)
		return RG_EINVAL;

	for (x = 0; x < target_list_len; x++) {

		target = target_list[x];
		ret = svc_start_remote(svcName, RG_START_REMOTE,
				       target);
		switch (ret) {
		case RG_ERUN:
			/* Someone stole the service while we were 
			   trying to start it */
			get_rg_state_local(svcName, &svcStatus);
			if (new_owner)
				*new_owner = svcStatus.rs_owner;
			return 0;
		case RG_EDEPEND:
		case RG_EFAIL:
			continue;
		case RG_EABORT:
			svc_report_failure(svcName);
			return RG_EFAIL;
		default:
			/* deliberate fallthrough */
			clulog(LOG_ERR,
			       "#61: Invalid reply from member %d during"
			       " start operation!\n", target);
		case RG_NO:
			/* state uncertain */
			clulog(LOG_CRIT, "State Uncertain: svc:%s "
			       "nid:%d req:%s ret:%d\n", svcName,
			       target, rg_req_str(RG_START_REMOTE), ret);
			return 0;
		case 0:
			if (new_owner)
				*new_owner = target;
			clulog(LOG_NOTICE, "Service %s is now running "
			       "on member %d\n", svcName, (int)target);
			return 0;
		}
	}

	return RG_EFAIL;
}


int
service_op_stop(char *svcName, int do_disable, int event_type)
{
	SmMessageSt msg;
	int msg_ret;
	msgctx_t ctx;
	rg_state_t svcStatus;
	int msgtarget = my_id();

	/* Build the message header */
	msg.sm_hdr.gh_magic = GENERIC_HDR_MAGIC;
	msg.sm_hdr.gh_command = RG_ACTION_REQUEST;
	msg.sm_hdr.gh_arg1 = RG_ACTION_MASTER; 
	msg.sm_hdr.gh_length = sizeof (SmMessageSt);

	msg.sm_data.d_action = ((!do_disable) ? RG_STOP:RG_DISABLE);

	if (msg.sm_data.d_action == RG_STOP && event_type == EVENT_USER)
		msg.sm_data.d_action = RG_STOP_USER;

	strncpy(msg.sm_data.d_svcName, svcName,
		sizeof(msg.sm_data.d_svcName));
	msg.sm_data.d_ret = 0;
	msg.sm_data.d_svcOwner = 0;

	/* Open a connection to the local node - it will decide what to
	   do in this case. XXX inefficient; should queue requests
	   locally and immediately forward requests otherwise */

	if (get_service_state_internal(svcName, &svcStatus) < 0)
		return RG_EFAIL;
	if (svcStatus.rs_owner > 0)
		msgtarget = svcStatus.rs_owner;

	if (msg_open(MSG_CLUSTER, msgtarget, RG_PORT, &ctx, 2)< 0) {
		clulog(LOG_ERR,
		       "#58: Failed opening connection to member #%d\n",
		       my_id());
		return -1;
	}

	/* Encode */
	swab_SmMessageSt(&msg);

	/* Send stop message to the other node */
	if (msg_send(&ctx, &msg, sizeof (SmMessageSt)) < 
	    (int)sizeof (SmMessageSt)) {
		clulog(LOG_ERR, "Failed to send complete message\n");
		msg_close(&ctx);
		return -1;
	}

	/* Check the response */
	do {
		msg_ret = msg_receive(&ctx, &msg,
				      sizeof (SmMessageSt), 10);
		if ((msg_ret == -1 && errno != ETIMEDOUT) ||
		    (msg_ret >= 0)) {
			break;
		}
	} while(1);

	if (msg_ret != sizeof (SmMessageSt)) {
		clulog(LOG_WARNING, "Strange response size: %d vs %d\n",
		       msg_ret, (int)sizeof(SmMessageSt));
		return 0;	/* XXX really UNKNOWN */
	}

	/* Got a valid response from other node. */
	msg_close(&ctx);

	/* Decode */
	swab_SmMessageSt(&msg);

	return msg.sm_data.d_ret;
}


/*
   TODO
   service_op_migrate()
 */


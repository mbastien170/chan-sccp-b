
/*!
 * \file        sccp_pbx.c
 * \brief       SCCP PBX Asterisk Wrapper Class
 * \author      Diederik de Groot <ddegroot [at] users.sourceforge.net>
 * \note        Reworked, but based on chan_sccp code.
 *              The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 *              Modified by Jan Czmok and Julien Goodwin
 * \note        This program is free software and may be modified and distributed under the terms of the GNU Public License.
 *              See the LICENSE file at the top of the source tree.
 *
 * $Date$
 * $Revision$  
 */
#ifndef __PBX_IMPL_C
#    define __PBX_IMPL_C

#    include <config.h>
#    include "common.h"

SCCP_FILE_VERSION(__FILE__, "$Revision$")

/*!
 * \brief SCCP Structure to pass data to the pbx answer thread 
 */
struct sccp_answer_conveyor_struct {
	uint32_t callid;
	sccp_linedevices_t *linedevice;
};

/*!
 * \brief Call Auto Answer Thead
 * \param data Data
 *
 * The Auto Answer thread is started by ref sccp_pbx_call if necessary
 */
static void *sccp_pbx_call_autoanswer_thread(void *data)
{
	struct sccp_answer_conveyor_struct *conveyor = data;

	sccp_channel_t *c = NULL;
	sccp_device_t *device = NULL;

	int instance = 0;

	sleep(GLOB(autoanswer_ring_time));
	pthread_testcancel();

	if (!conveyor) {
		return NULL;
	}
	if (!conveyor->linedevice) {
		goto FINAL;
	}
	if (!(device = sccp_device_retain(conveyor->linedevice->device))) {
		goto FINAL;
	}

	if (!(c = sccp_channel_find_byid(conveyor->callid))) {
		goto FINAL;
	}

	if (c->state != SCCP_CHANNELSTATE_RINGING) {
		goto FINAL;
	}

	sccp_channel_answer(device, c);

	if (GLOB(autoanswer_tone) != SKINNY_TONE_SILENCE && GLOB(autoanswer_tone) != SKINNY_TONE_NOTONE) {
		instance = sccp_device_find_index_for_line(device, c->line->name);
		sccp_dev_starttone(device, GLOB(autoanswer_tone), instance, c->callid, 0);
	}
	if (c->autoanswer_type == SCCP_AUTOANSWER_1W)
		sccp_dev_set_microphone(device, SKINNY_STATIONMIC_OFF);

 FINAL:
	c = c ? sccp_channel_release(c) : NULL;
	device = device ? sccp_device_release(device) : NULL;
	conveyor->linedevice = conveyor->linedevice ? sccp_linedevice_release(conveyor->linedevice) : NULL;	// retained in calling thread, final release here
	sccp_free(conveyor);
	return NULL;
}

/*!
 * \brief Incoming Calls by Asterisk SCCP_Request
 * \param c		SCCP Channel
 * \param dest 		Destination as char
 * \param timeout 	Timeout after which incoming call is cancelled as int
 * \return Success as int
 *
 * \todo reimplement DNDMODES, ringermode=urgent, autoanswer
 *
 * \callgraph
 * \callergraph
 * 
 * \called_from_asterisk
 * 
 * \lock
 * 	- line
 * 	  - line->devices
 * 	- line
 * 	- line->devices
 * 	  - see sccp_device_sendcallstate()
 * 	  - see sccp_channel_send_callinfo()
 * 	  - see sccp_channel_forward()
 * 	  - see sccp_util_matchSubscriptionId()
 * 	  - see sccp_channel_get_active()
 * 	  - see sccp_indicate_lock()
 * 
 * \note called with c retained
 */

int sccp_pbx_call(sccp_channel_t * c, char *dest, int timeout)
{
	sccp_line_t *l;
	sccp_channel_t *active_channel = NULL;

	char *cid_name = NULL;
	char *cid_number = NULL;

	char suffixedNumber[255] = { '\0' };									/*!< For saving the digittimeoutchar to the logs */
	boolean_t hasSession = FALSE;

	l = sccp_line_retain(c->line);
	if (l) {
		sccp_linedevices_t *linedevice;

		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
			assert(linedevice->device);

			if (linedevice->device->session)
				hasSession = TRUE;
		}
		SCCP_LIST_UNLOCK(&l->devices);
		if (!hasSession) {
			pbx_log(LOG_WARNING, "SCCP: weird error. The channel %d has no device connected to this line or device has no valid session\n", (c ? c->callid : 0));
			if (l)
				l = sccp_line_release(l);
			return -1;
		}
	} else {
		pbx_log(LOG_WARNING, "SCCP: weird error. The channel %d has no line\n", (c ? c->callid : 0));
		if (l)
			l = sccp_line_release(l);
		return -1;
	}

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Asterisk request to call %s\n", l->id, PBX(getChannelName) (c));

	/* if incoming call limit is reached send BUSY */
	if (SCCP_RWLIST_GETSIZE(l->channels) > l->incominglimit) {						/* >= just to be sure :-) */
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "Incoming calls limit (%d) reached on SCCP/%s... sending busy\n", l->incominglimit, l->name);
		l = sccp_line_release(l);
		pbx_setstate(c->owner, AST_STATE_BUSY);
		PBX(queue_control) (c->owner, AST_CONTROL_BUSY);
		return 0;
	}

	/* Reinstated this call instead of the following lines */
	if (strlen(c->callInfo.callingPartyName) > 0)
		cid_name = strdup(c->callInfo.callingPartyName);

	if (strlen(c->callInfo.callingPartyNumber) > 0)
		cid_number = strdup(c->callInfo.callingPartyNumber);

	//! \todo implement dnid, ani, ani2 and rdnis
	sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_call) asterisk callerid='%s <%s>'\n", (cid_number) ? cid_number : "", (cid_name) ? cid_name : "");

	/* Set the channel callingParty Name and Number, called Party Name and Number, original CalledParty Name and Number, Presentation */
	if (GLOB(recorddigittimeoutchar)) {
		/* The hack to add the # at the end of the incoming number
		   is only applied for numbers beginning with a 0,
		   which is appropriate for Germany and other countries using similar numbering plan.
		   The option should be generalized, moved to the dialplan, or otherwise be replaced. */
		/* Also, we require an option whether to add the timeout suffix to certain
		   enbloc dialed numbers (such as via 7970 enbloc dialing) if they match a certain pattern.
		   This would help users dial from call history lists on other phones, which do not have enbloc dialing,
		   when using shared lines. */
		if (NULL != cid_number && strlen(cid_number) > 0 && strlen(cid_number) < sizeof(suffixedNumber) - 2 && '0' == cid_number[0]) {
			strncpy(suffixedNumber, cid_number, strlen(cid_number));
			suffixedNumber[strlen(cid_number) + 0] = '#';
			suffixedNumber[strlen(cid_number) + 1] = '\0';
			sccp_channel_set_callingparty(c, cid_name, suffixedNumber);
		} else
			sccp_channel_set_callingparty(c, cid_name, cid_number);

	} else {
		sccp_channel_set_callingparty(c, cid_name, cid_number);
	}
	/* Set the channel calledParty Name and Number 7910 compatibility */
	sccp_channel_set_calledparty(c, l->cid_name, l->cid_num);
	PBX(set_connected_line) (c, c->callInfo.calledPartyNumber, c->callInfo.calledPartyName, AST_CONNECTED_LINE_UPDATE_SOURCE_TRANSFER_ALERTING);

	//! \todo implement dnid, ani, ani2 and rdnis
	if (PBX(get_callerid_presence)) {
		c->callInfo.presentation = PBX(get_callerid_presence) (c);
	}

	sccp_channel_display_callInfo(c);

	if (!c->ringermode) {
		c->ringermode = SKINNY_STATION_OUTSIDERING;
		//ringermode = pbx_builtin_getvar_helper(c->owner, "ALERT_INFO");
	}

/*
	if (l->devices.size == 1 && SCCP_LIST_FIRST(&l->devices) && SCCP_LIST_FIRST(&l->devices)->device && SCCP_LIST_FIRST(&l->devices)->device->session) {
		//! \todo check if we have to do this
		sccp_channel_setDevice(c, SCCP_LIST_FIRST(&l->devices)->device);
//              c->device = SCCP_LIST_FIRST(&l->devices)->device;
		sccp_channel_updateChannelCapability(c);
	}
*/

	boolean_t isRinging = FALSE;
	boolean_t hasDNDParticipant = FALSE;

	sccp_linedevices_t *linedevice;

	SCCP_LIST_LOCK(&l->devices);
	SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
		assert(linedevice->device);

		/* do we have cfwd enabled? */
		if (linedevice->cfwdAll.enabled) {
			pbx_log(LOG_NOTICE, "%s: initialize cfwd for line %s\n", linedevice->device->id, l->name);
			if (sccp_channel_forward(c, linedevice, linedevice->cfwdAll.number) == 0) {
				sccp_device_sendcallstate(linedevice->device, linedevice->lineInstance, c->callid, SKINNY_CALLSTATE_INTERCOMONEWAY, SKINNY_CALLPRIORITY_NORMAL, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
				sccp_channel_send_callinfo(linedevice->device, c);
#    ifdef CS_EXPERIMENTAL
				if (sccp_strlen_zero(pbx_builtin_getvar_helper(c->owner, "FORWARDER_FOR"))) {
					struct ast_var_t *variables;
					const char *var, *val;
					char mask[25];

					ast_channel_lock(c->owner);
					sprintf(mask, "SCCP::%d", c->callid);
					AST_LIST_TRAVERSE(&c->owner->varshead, variables, entries) {
						if ((var = ast_var_name(variables)) && (val = ast_var_value(variables)) && (!strcmp("LINKID", var)) && (strcmp(mask, val))) {
							sccp_log(DEBUGCAT_CORE) (VERBOSE_PREFIX_1 "SCCP: LINKID %s\n", val);
							pbx_builtin_setvar_helper(c->owner, "__FORWARDER_FOR", val);
						}
					}
					ast_channel_unlock(c->owner);
				}
#    endif
				isRinging = TRUE;
			}
			continue;
		}

		if (!linedevice->device->session)
			continue;

		/* check if c->subscriptionId.number is matching deviceSubscriptionID */
		/* This means that we call only those devices on a shared line
		   which match the specified subscription id in the dial parameters. */
		if (!sccp_util_matchSubscriptionId(c, linedevice->subscriptionId.number)) {
			continue;
		}

		if ((active_channel = sccp_channel_get_active(linedevice->device))) {
                        sccp_indicate(linedevice->device, c, SCCP_CHANNELSTATE_CALLWAITING);
                        isRinging = TRUE;
			active_channel = sccp_channel_release(active_channel);
		} else {
			if (linedevice->device->dndFeature.enabled && linedevice->device->dndFeature.status == SCCP_DNDMODE_REJECT) {
				hasDNDParticipant = TRUE;
				continue;
			}
			sccp_indicate(linedevice->device, c, SCCP_CHANNELSTATE_RINGING);
			isRinging = TRUE;
			if (c->autoanswer_type) {

				struct sccp_answer_conveyor_struct *conveyor = sccp_calloc(1, sizeof(struct sccp_answer_conveyor_struct));

				if (conveyor) {
					sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: Running the autoanswer thread on %s\n", DEV_ID_LOG(linedevice->device), PBX(getChannelName) (c));
					conveyor->callid = c->callid;
					conveyor->linedevice = sccp_linedevice_retain(linedevice);

#    if !CS_EXPERIMENTAL											/* new default */
					sccp_threadpool_add_work(GLOB(general_threadpool), (void *)sccp_pbx_call_autoanswer_thread, (void *)conveyor);
#    else
					pthread_t t;
					pthread_attr_t attr;

					pthread_attr_init(&attr);
					pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
					if (pbx_pthread_create(&t, &attr, sccp_pbx_call_autoanswer_thread, conveyor)) {
						pbx_log(LOG_WARNING, "%s: Unable to create switch thread for channel (%s-%08x) %s\n", DEV_ID_LOG(linedevice->device), l->name, c->callid, strerror(errno));
						sccp_free(conveyor);
					}
					pthread_detach(t);
					pthread_attr_destroy(&attr);
#    endif
				}
			}
		}
	}
	SCCP_LIST_UNLOCK(&l->devices);

	if (isRinging) {
		sccp_channel_setSkinnyCallstate(c, SKINNY_CALLSTATE_RINGIN);
		PBX(queue_control) (c->owner, AST_CONTROL_RINGING);
	} else if (hasDNDParticipant) {
		PBX(queue_control) (c->owner, AST_CONTROL_BUSY);
	} else {
		PBX(queue_control) (c->owner, AST_CONTROL_CONGESTION);
	}

	if (cid_name)
		free(cid_name);
	if (cid_number)
		free(cid_number);

	/** 
	 * workaround to fix: 
	 * [Jun 21 08:44:15] WARNING[21040]: channel.c:4934 ast_write: Codec mismatch on channel SCCP/109-0000000a setting write format to slin16 from ulaw native formats 0x0 (nothing) 
	 * 
	 */
	PBX(rtp_setWriteFormat) (c, SKINNY_CODEC_WIDEBAND_256K);
	PBX(rtp_setReadFormat) (c, SKINNY_CODEC_WIDEBAND_256K);

	l = sccp_line_release(l);

	return isRinging != TRUE;
}

/*!
 * \brief Handle Hangup Request by Asterisk
 * \param c SCCP Channel
 * \return Success as int
 *
 * \callgraph
 * \callergraph
 * 
 * \called_from_asterisk via pbx_impl\ast\ast....c:sccp_wrapper_asterisk.._hangup
 * 
 * \note sccp_channel should be retained in calling function
 */

int sccp_pbx_hangup(sccp_channel_t * c)
{
	sccp_line_t *l = NULL;
	sccp_device_t *d = NULL;

	/* here the ast channel is locked */
	//sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Asterisk request to hangup channel %s\n", PBX(getChannelName)(c));

	sccp_mutex_lock(&GLOB(usecnt_lock));
	GLOB(usecnt)--;
	sccp_mutex_unlock(&GLOB(usecnt_lock));

	pbx_update_use_count();

	if (!(c = sccp_channel_retain(c))) {
		sccp_log((DEBUGCAT_PBX + DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: Asked to hangup channel. SCCP channel already deleted\n");
		sccp_pbx_needcheckringback(d);
		return -1;
	}

	d = sccp_channel_getDevice_retained(c);
	if (d && c->state != SCCP_CHANNELSTATE_DOWN && SKINNY_DEVICE_RS_OK == d->registrationState) {
		//if (GLOB(remotehangup_tone) && d && d->state == SCCP_DEVICESTATE_OFFHOOK && c == sccp_channel_get_active_nolock(d))           /* Caused active channels never to be full released */
		if (GLOB(remotehangup_tone) && d && d->state == SCCP_DEVICESTATE_OFFHOOK && c == d->active_channel)
			sccp_dev_starttone(d, GLOB(remotehangup_tone), 0, 0, 10);
		sccp_indicate(d, c, SCCP_CHANNELSTATE_ONHOOK);
	}

	c->owner = NULL;
	l = sccp_line_retain(c->line);
#    ifdef CS_SCCP_CONFERENCE
	if (c->conference) {
		//sccp_conference_removeParticipant(c->conference, c);
		sccp_conference_retractParticipatingChannel(c->conference, c);
	}
#    endif									// CS_SCCP_CONFERENCE

	if (c) {
		if (c->rtp.audio.rtp || c->rtp.video.rtp) {
			if (d && SKINNY_DEVICE_RS_OK == d->registrationState)
				sccp_channel_closereceivechannel(c);
			sccp_rtp_destroy(c);
		}
	}
	// removing scheduled dialing
	c->scheduler.digittimeout = SCCP_SCHED_DEL(c->scheduler.digittimeout);

	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "%s: Current channel %s-%08x state %s(%d)\n", (d) ? DEV_ID_LOG(d) : "(null)", l ? l->name : "(null)", c->callid, sccp_indicate2str(c->state), c->state);

	/* end callforwards */
	sccp_channel_t *channel;

	SCCP_LIST_LOCK(&l->channels);
	SCCP_LIST_TRAVERSE(&l->channels, channel, list) {
		if (channel->parentChannel == c) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: PBX Hangup cfwd channel %s-%08X\n", DEV_ID_LOG(d), l->name, channel->callid);
			/* No need to lock because c->line->channels is already locked. */
			sccp_channel_endcall(channel);
		}
	}
	SCCP_LIST_UNLOCK(&l->channels);

	sccp_line_removeChannel(l, c);

	if (!d) {
		/* channel is not answered, just ringin over all devices */
		sccp_linedevices_t *linedevice;

		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
			if (linedevice->device && SKINNY_DEVICE_RS_OK == linedevice->device->registrationState && (d = sccp_device_retain(linedevice->device))) {
				sccp_indicate(d, c, SKINNY_CALLSTATE_ONHOOK);
				d = sccp_device_release(d);
			}
		}
		SCCP_LIST_UNLOCK(&l->devices);
		d = NULL;											/* do not use any device within this loop, otherwise we get a "Major Logic Error" -MC */
	} else if (SKINNY_DEVICE_RS_OK != d->registrationState) {
		c->state = SCCP_CHANNELSTATE_DOWN;								// device is reregistering
	} else {
		/* 
		 * Really neccessary?
		 * Test for 7910 (to remove the following line)
		 *  (-DD)
		 */
		sccp_channel_send_callinfo(d, c);
		sccp_pbx_needcheckringback(d);
		sccp_dev_check_displayprompt(d);
	}

	sccp_channel_clean(c);

//      if (sccp_sched_add(0, sccp_channel_destroy_callback, c) < 0) {
//              sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: Unable to schedule destroy of channel %08X\n", c->callid);
//      }
	sccp_channel_destroy(c);

	d = d ? sccp_device_release(d) : NULL;
	l = l ? sccp_line_release(l) : NULL;
	c = c ? sccp_channel_release(c) : NULL;

	return 0;
}

/*!
 * \brief Thread to check Device Ring Back
 *
 * The Auto Answer thread is started by ref sccp_pbx_needcheckringback if necessary
 *
 * \param d SCCP Device
 * 
 * \lock
 * 	- device->session
 */
void sccp_pbx_needcheckringback(sccp_device_t * d)
{

	if (d && d->session) {
		sccp_session_lock(d->session);
		d->session->needcheckringback = 1;
		sccp_session_unlock(d->session);
	}
}

/*!
 * \brief Answer an Asterisk Channel
 * \note we have no bridged channel at this point
 *
 * \param c SCCCP channel
 * \return Success as int
 *
 * \callgraph
 * \callergraph
 *
 * \called_from_asterisk
 *
 * \todo masquarade does not succeed when forwarding to a dialplan extension which starts with PLAYBACK (Is this still the case, i think this might have been resolved ?? - DdG -)
 */
int sccp_pbx_answer(sccp_channel_t * channel)
{
	sccp_channel_t *c = NULL;

	int res = 0;

	sccp_log((DEBUGCAT_PBX | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "SCCP: sccp_pbx_answer\n");

	/* \todo perhaps we should lock channel here. */
	if (!(c = sccp_channel_retain(channel)))
		return -1;

	sccp_log((DEBUGCAT_PBX | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: sccp_pbx_answer checking parent channel\n", c->currentDeviceId);
	if (c->parentChannel) {											// containing a retained channel, final release at the end
		/* we are a forwarded call, bridge me with my parent */
		sccp_log((DEBUGCAT_PBX | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_3 "%s: bridge me with my parent's channel %s\n", c->currentDeviceId, PBX(getChannelName) (c));

		PBX_CHANNEL_TYPE *br = NULL, *astForwardedChannel = c->parentChannel->owner;

		if (PBX(getChannelAppl) (c)) {
			sccp_log((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_answer) %s bridging to dialplan application %s\n", c->currentDeviceId, PBX(getChannelName) (c), PBX(getChannelAppl) (c));
		}

		/* at this point we do not have a pointer to ou bridge channel so we search for it -MC */
		const char *bridgePeerChannelName = pbx_builtin_getvar_helper(c->owner, "BRIDGEPEER");

		if (!sccp_strlen_zero(bridgePeerChannelName)) {
			sccp_log((DEBUGCAT_PBX + DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer) searching for bridgepeer by name: %s\n", bridgePeerChannelName);
			PBX(getChannelByName) (bridgePeerChannelName, &br);
		}

		/* did we find our bridge */
		pbx_log(LOG_NOTICE, "SCCP: bridge: %s\n", (br) ? pbx_channel_name(br) : " -- no bridgepeer found -- ");
		if (br) {
			/* set the channel and the bridge to state UP to fix problem with fast pickup / autoanswer */
			pbx_setstate(c->owner, AST_STATE_UP);
			pbx_setstate(br, AST_STATE_UP);

			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer) Going to Masquerade %s into %s\n", pbx_channel_name(br), pbx_channel_name(astForwardedChannel));
			if (!pbx_channel_masquerade(astForwardedChannel, br)) {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer) Masqueraded into %s\n", pbx_channel_name(astForwardedChannel));
				sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) bridged. channel state: ast %s\n", pbx_state2str(pbx_channel_state(c->owner)));
				sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) bridged. channel state: astForwardedChannel %s\n", pbx_state2str(pbx_channel_state(astForwardedChannel)));
				sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) bridged. channel state: br %s\n", pbx_state2str(pbx_channel_state(br)));
				sccp_log((DEBUGCAT_HIGH)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) ============================================== \n");
			} else {
				pbx_log(LOG_ERROR, "(sccp_pbx_answer) Failed to masquerade bridge into forwarded channel\n");
				res = -1;
			}
		} else {
			/* we have no bridge and can not make a masquerade -> end call */
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) no bridge. channel state: ast %s\n", pbx_state2str(pbx_channel_state(c->owner)));
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) no bridge. channel state: astForwardedChannel %s\n", pbx_state2str(pbx_channel_state(astForwardedChannel)));
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "(sccp_pbx_answer: call forward) ============================================== \n");

			if (pbx_channel_state(c->owner) == AST_STATE_RING && pbx_channel_state(astForwardedChannel) == AST_STATE_DOWN && PBX(getChannelPbx) (c)) {
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_4 "SCCP: Receiver Hungup: (hasPBX: %s)\n", PBX(getChannelPbx) (c) ? "yes" : "no");
				pbx_channel_set_hangupcause(astForwardedChannel, AST_CAUSE_CALL_REJECTED);
				//astForwardedChannel->_softhangup |= AST_SOFTHANGUP_DEV;
				pbx_queue_hangup(astForwardedChannel);
			} else {
				pbx_log(LOG_ERROR, "%s: We did not find bridge channel for call forwarding call. Hangup\n", c->currentDeviceId);
				pbx_channel_set_hangupcause(astForwardedChannel, AST_CAUSE_REQUESTED_CHAN_UNAVAIL);
//                              astForwardedChannel->_softhangup |= AST_SOFTHANGUP_DEV;
				pbx_queue_hangup(astForwardedChannel);
				sccp_channel_endcall(c);
				res = -1;
			}
		}
		c->parentChannel = sccp_channel_release(c->parentChannel);					// release parentChannel, freeing reference
		// FINISH
	} else {
		sccp_device_t *d = NULL;

		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Outgoing call has been answered %s on %s@%s-%08x\n", PBX(getChannelName) (c), c->line->name, c->currentDeviceId, c->callid);
		sccp_channel_updateChannelCapability(c);

		/*! \todo This seems like brute force, and doesn't seem to be of much use. However, I want it to be remebered
		   as I have forgotten what my actual motivation was for writing this strange code. (-DD) */
		if ((d = sccp_channel_getDevice_retained(c))) {
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_channel_send_callinfo(d, c);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_PROCEED);
			sccp_channel_send_callinfo(d, c);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_CONNECTED);
			d = sccp_device_release(d);
		}

		if (c->rtp.video.writeState & SCCP_RTP_STATUS_ACTIVE) {
			PBX(queue_control) (c->owner, AST_CONTROL_VIDUPDATE);
		}
		// FINISH
	}

//FINISH:
	c = sccp_channel_release(c);
	return res;
}

/*!
 * \brief Allocate an Asterisk Channel
 * \param c SCCP Channel
 * \return 1 on Success as uint8_t
 *
 * \callgraph
 * \callergraph
 * 
 * \lock
 * 	- line->devices
 * 	- channel
 * 	  - line
 * 	  - see sccp_channel_updateChannelCapability()
 * 	- usecnt_lock
 */
uint8_t sccp_pbx_channel_allocate(sccp_channel_t * c)
{
	PBX_CHANNEL_TYPE *tmp;

	if (!c)
		return -1;

	sccp_line_t *l = sccp_line_retain(c->line);
	sccp_device_t *d = NULL;

#    ifndef CS_AST_CHANNEL_HAS_CID
	char cidtmp[256];

	memset(&cidtmp, 0, sizeof(cidtmp));
#    endif									// CS_AST_CHANNEL_HAS_CID

	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: try to allocate channel \n");
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: Line: %s\n", l->name);

	if (!l) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Unable to allocate asterisk channel %s\n", l->name);
		pbx_log(LOG_ERROR, "SCCP: Unable to allocate asterisk channel\n");
		return 0;
	}
//      /* Don't hold a sccp pvt lock while we allocate a channel */
	if ((d = sccp_channel_getDevice_retained(c))) {
		sccp_linedevices_t *linedevice;

		SCCP_LIST_LOCK(&l->devices);
		SCCP_LIST_TRAVERSE(&l->devices, linedevice, list) {
			if (linedevice->device == d)
				break;
		}
		SCCP_LIST_UNLOCK(&l->devices);

		switch (c->calltype) {
			case SKINNY_CALLTYPE_INBOUND:
				/* append subscriptionId to cid */
				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.number)) {
					sprintf(c->callInfo.calledPartyNumber, "%s%s", l->cid_num, linedevice->subscriptionId.number);
				} else {
					sprintf(c->callInfo.calledPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				}

				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.name)) {
					sprintf(c->callInfo.calledPartyName, "%s%s", l->cid_name, linedevice->subscriptionId.name);
				} else {
					sprintf(c->callInfo.calledPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				}
				break;
			case SKINNY_CALLTYPE_FORWARD:
			case SKINNY_CALLTYPE_OUTBOUND:
				/* append subscriptionId to cid */
				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.number)) {
					sprintf(c->callInfo.callingPartyNumber, "%s%s", l->cid_num, linedevice->subscriptionId.number);
				} else {
					sprintf(c->callInfo.callingPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				}

				if (linedevice && !sccp_strlen_zero(linedevice->subscriptionId.name)) {
					sprintf(c->callInfo.callingPartyName, "%s%s", l->cid_name, linedevice->subscriptionId.name);
				} else {
					sprintf(c->callInfo.callingPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				}
				break;
		}
	} else {

		switch (c->calltype) {
			case SKINNY_CALLTYPE_INBOUND:
				sprintf(c->callInfo.calledPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				sprintf(c->callInfo.calledPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				break;
			case SKINNY_CALLTYPE_FORWARD:
			case SKINNY_CALLTYPE_OUTBOUND:
				sprintf(c->callInfo.callingPartyNumber, "%s%s", l->cid_num, (l->defaultSubscriptionId.number) ? l->defaultSubscriptionId.number : "");
				sprintf(c->callInfo.callingPartyName, "%s%s", l->cid_name, (l->defaultSubscriptionId.name) ? l->defaultSubscriptionId.name : "");
				break;
		}
	}

	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:     cid_num: \"%s\"\n", c->callInfo.callingPartyNumber);
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:    cid_name: \"%s\"\n", c->callInfo.callingPartyName);
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP: accountcode: \"%s\"\n", l->accountcode);
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:       exten: \"%s\"\n", c->dialedNumber);
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:     context: \"%s\"\n", l->context);
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:    amaflags: \"%d\"\n", l->amaflags);
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "SCCP:   chan/call: \"%s-%08x\"\n", l->name, c->callid);

	/* This should definitely fix CDR */
//      tmp = pbx_channel_alloc(1, AST_STATE_DOWN, c->callInfo.callingPartyNumber, c->callInfo.callingPartyName, l->accountcode, c->dialedNumber, l->context, l->amaflags, "SCCP/%s-%08x", l->name, c->callid);
	PBX(alloc_pbxChannel) (c, &tmp);
	if (!tmp) {
		pbx_log(LOG_ERROR, "%s: Unable to allocate asterisk channel on line %s\n", l->id, l->name);
		l = l ? sccp_line_release(l) : NULL;
		d = d ? sccp_device_release(d) : NULL;
		return 0;
	}

	sccp_channel_updateChannelCapability(c);
	PBX(set_nativeAudioFormats) (c, c->preferences.audio, 1);

	//! \todo check locking
	/* \todo we should remove this shit. */
	char tmpName[StationMaxNameSize];

	snprintf(tmpName, sizeof(tmpName), "SCCP/%s-%08x", l->name, c->callid);
	PBX(setChannelName) (c, tmpName);

	pbx_jb_configure(tmp, &GLOB(global_jbconf));

	// \todo: Bridge?
	// \todo: Transfer?
	sccp_mutex_lock(&GLOB(usecnt_lock));
	GLOB(usecnt)++;
	sccp_mutex_unlock(&GLOB(usecnt_lock));

	pbx_update_use_count();

	if (PBX(set_callerid_number))
		PBX(set_callerid_number) (c, c->callInfo.callingPartyNumber);

	if (PBX(set_callerid_name))
		PBX(set_callerid_name) (c, c->callInfo.callingPartyName);

//      if (d && d->monitorFeature.status == SCCP_FEATURE_MONITOR_STATE_ENABLED_NOTACTIVE) {

	/** check for monitor request */
	if (d && (d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_REQUESTED)
	    && !(d->monitorFeature.status & SCCP_FEATURE_MONITOR_STATE_ACTIVE)) {

		sccp_feat_monitor(d, c->line, 0, c);
		sccp_feat_changed(d, SCCP_FEATURE_MONITOR);
	}

	/* asterisk needs the native formats bevore dialout, otherwise the next channel gets the whole AUDIO_MASK as requested format
	 * chan_sip dont like this do sdp processing */
//      PBX(set_nativeAudioFormats)(c, c->preferences.audio, ARRAY_LEN(c->preferences.audio));

	// export sccp informations in asterisk dialplan
	if (d) {
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_MAC", d->id);
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_IP", pbx_inet_ntoa(d->session->sin.sin_addr));
		pbx_builtin_setvar_helper(tmp, "SCCP_DEVICE_TYPE", devicetype2str(d->skinny_type));
	}
	sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_3 "%s: Allocated asterisk channel %s-%d\n", (l) ? l->id : "(null)", (l) ? l->name : "(null)", c->callid);

	l = l ? sccp_line_release(l) : NULL;
	d = d ? sccp_device_release(d) : NULL;
	return 1;
}

/*!
 * \brief Schedule Asterisk Dial
 * \param data Data as constant
 * \return Success as int
 * 
 * \called_from_asterisk
 */
int sccp_pbx_sched_dial(const void *data)
{
	sccp_channel_t *c = NULL;

	if ((c = sccp_channel_retain((sccp_channel_t *) data))) {
		if (c->owner && !PBX(getChannelPbx) (c)) {
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: Timeout for call '%d'. Going to dial '%s'\n", c->callid, c->dialedNumber);
			sccp_pbx_softswitch(c);
		}
		sccp_channel_release(c);
	}
	return 0;
}

/*!
 * \brief Asterisk Helper
 * \param c SCCP Channel as sccp_channel_t
 * \return Success as int
 */
sccp_extension_status_t sccp_pbx_helper(sccp_channel_t * c)
{
	sccp_extension_status_t extensionStatus;
	sccp_device_t *d = NULL;

	if (!sccp_strlen_zero(c->dialedNumber)) {
		if (GLOB(recorddigittimeoutchar) && GLOB(digittimeoutchar) == c->dialedNumber[strlen(c->dialedNumber) - 1]) {
			/* we finished dialing with digit timeout char */
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: We finished dialing with digit timeout char %s\n", c->dialedNumber);
			return SCCP_EXTENSION_EXACTMATCH;
		}
	}

	if ((c->ss_action != SCCP_SS_GETCBARGEROOM) && (c->ss_action != SCCP_SS_GETMEETMEROOM)) {

		//! \todo check overlap feature status -MC
		extensionStatus = PBX(extension_status) (c);
		if ((d = sccp_channel_getDevice_retained(c))) {
			if (((d->overlapFeature.enabled && !extensionStatus) || (!d->overlapFeature.enabled && !extensionStatus))
			    && ((d->overlapFeature.enabled && !extensionStatus) || (!d->overlapFeature.enabled && !extensionStatus))) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: %s Matches more\n", c->dialedNumber);
				d = sccp_device_release(d);
				return SCCP_EXTENSION_MATCHMORE;
			}
			sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: %s Match %s\n", c->dialedNumber, extensionStatus == SCCP_EXTENSION_EXACTMATCH ? "Exact" : "More");
			d = sccp_device_release(d);
		}
		return extensionStatus;
	}
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "SCCP: %s Does Exists\n", c->dialedNumber);
	return SCCP_EXTENSION_NOTEXISTS;
}

/*!
 * \brief Handle Soft Switch
 * \param c SCCP Channel as sccp_channel_t
 * \todo clarify Soft Switch Function
 *
 * \lock
 * 	- channel
 * 	  - see sccp_pbx_senddigits()
 * 	  - see sccp_channel_set_calledparty()
 * 	  - see sccp_indicate_nolock()
 * 	- channel
 * 	  - see sccp_line_cfwd()
 * 	  - see sccp_indicate_nolock()
 * 	  - see sccp_device_sendcallstate()
 * 	  - see sccp_channel_send_callinfo()
 * 	  - see sccp_dev_clearprompt()
 * 	  - see sccp_dev_displayprompt()
 * 	  - see sccp_feat_meetme_start()
 * 	  - see PBX(set_callstate)()
 * 	  - see pbx_pbx_start()
 * 	  - see sccp_indicate_nolock()
 * 	  - see manager_event()
 */
void *sccp_pbx_softswitch(sccp_channel_t * c)
{
#    if DDGNEW
	PBX_CHANNEL_TYPE *chan = NULL;

	if (!c) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <channel> available. Returning from dial thread.\n");
		return NULL;
	}
	sccp_device_t *d = NULL;

	/* Reset Enbloc Dial Emulation */
	c->enbloc.deactivate = 0;
	c->enbloc.totaldigittime = 0;
	c->enbloc.totaldigittimesquared = 0;
	c->enbloc.digittimeout = GLOB(digittimeout) * 1000;

	/* prevent softswitch from being executed twice (Pavel Troller / 15-Oct-2010) */
	if (PBX(getChannelPbx) (c)) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_softswitch) PBX structure already exists. Dialing instead of starting.\n");
		/* If there are any digits, send them instead of starting the PBX */
		if (!sccp_strlen_zero(c->dialedNumber)) {
			sccp_pbx_senddigits(c, c->dialedNumber);
			sccp_channel_set_calledparty(c, c->dialedNumber, c->dialedNumber);
			if ((d = sccp_channel_getDevice_retained(c))) {
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				d = sccp_device_release(d);
			}
		}
		return NULL;
	}

	if (!c->owner) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No valid pbx_channel connected to this channel.\n");
		return NULL;
	} else {
		chan = c->owner;
	}

	if (pbx_check_hangup(chan)) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) pbx_channel already hungup before softswitch could commence.\n");
	}

	PBX_VARIABLE_TYPE *v = NULL;

	uint8_t instance;

	unsigned int len = 0;

	sccp_line_t *l;

	char shortenedNumber[256] = { '\0' };									/* For recording the digittimeoutchar */

	/* removing scheduled dialing */
	c->scheduler.digittimeout = SCCP_SCHED_DEL(c->scheduler.digittimeout);

	/* we should just process outbound calls, let's check calltype */
	if (c->calltype != SKINNY_CALLTYPE_OUTBOUND) {
		d = sccp_device_release(d);
		return NULL;
	}

	/* assume d is the channel's device */
	/* does it exists ? */
	if (!(d = sccp_channel_getDevice_retained(c))) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <device> available. Returning from dial thread.\n");
		return NULL;
	}

	/* we don't need to check for a device type but just if the device has an id, otherwise back home  -FS */
	if (!d->id || sccp_strlen_zero(d->id)) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <device> identifier available. Returning from dial thread.\n");
		d = sccp_device_release(d);
		return NULL;
	}

	l = c->line;
	if (!l) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <line> available. Returning from dial thread.\n");
		if (chan) {
			PBX(requestHangup) (chan);
		}
		d = sccp_device_release(d);
		return NULL;
	}

	instance = sccp_device_find_index_for_line(d, c->line->name);
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) New call on line %s\n", DEV_ID_LOG(d), l->name);

	/* assign callerid name and number */
	//sccp_channel_set_callingparty(c, l->cid_name, l->cid_num);

	// we use shortenedNumber but why ???
	// If the timeout digit has been used to terminate the number
	// and this digit shall be included in the phone call history etc (recorddigittimeoutchar is true)
	// we still need to dial the number without the timeout char in the pbx
	// so that we don't dial strange extensions with a trailing characters.
	sccp_copy_string(shortenedNumber, c->dialedNumber, sizeof(shortenedNumber));
	len = strlen(shortenedNumber);
	assert(strlen(c->dialedNumber) == len);

	if (len > 0 && GLOB(digittimeoutchar) == shortenedNumber[len - 1]) {
		shortenedNumber[len - 1] = '\0';

		// If we don't record the timeoutchar in the logs, we remove it from the sccp channel structure
		// Later, the channel dialed number is used for directories, etc.,
		// and the shortened number is used for dialing the actual call via asterisk pbx.
		if (!GLOB(recorddigittimeoutchar)) {
			c->dialedNumber[len - 1] = '\0';
		}
	}

	/* This will choose what to do */
	switch (c->ss_action) {
		case SCCP_SS_GETFORWARDEXTEN:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Forward Extension\n", d->id);
			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_line_cfwd(l, d, c->ss_data, shortenedNumber);
			}
			sccp_channel_endcall(c);
			d = sccp_device_release(d);
			return NULL;										// leave simple switch without dial
#        ifdef CS_SCCP_PICKUP
		case SCCP_SS_GETPICKUPEXTEN:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Pickup Extension\n", d->id);
			// like we're dialing but we're not :)
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_channel_send_callinfo(d, c);
			sccp_dev_clearprompt(d, instance, c->callid);
			sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);

			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Asterisk request to pickup exten '%s'\n", shortenedNumber);
				if (sccp_feat_directpickup(c, shortenedNumber)) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				}
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
			}
			d = sccp_device_release(d);
			return NULL;										// leave simpleswitch without dial
#        endif									// CS_SCCP_PICKUP
		case SCCP_SS_GETMEETMEROOM:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme request\n", d->id);
			if (!sccp_strlen_zero(shortenedNumber) && !sccp_strlen_zero(c->line->meetmenum)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme request for room '%s' on extension '%s'\n", d->id, shortenedNumber, c->line->meetmenum);
				if (c->owner && !pbx_check_hangup(c->owner))
					pbx_builtin_setvar_helper(c->owner, "SCCP_MEETME_ROOM", shortenedNumber);
				sccp_copy_string(shortenedNumber, c->line->meetmenum, sizeof(shortenedNumber));

				//sccp_copy_string(c->dialedNumber, SKINNY_DISP_CONFERENCE, sizeof(c->dialedNumber));
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Start Meetme Thread\n", d->id);
				sccp_feat_meetme_start(c);							/* Copied from Federico Santulli */
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme Thread Started\n", d->id);
				d = sccp_device_release(d);
				return NULL;
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
				d = sccp_device_release(d);
				return NULL;
			}
			break;
		case SCCP_SS_GETBARGEEXTEN:
			// like we're dialing but we're not :)
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Barge Extension\n", d->id);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_channel_send_callinfo(d, c);

			sccp_dev_clearprompt(d, instance, c->callid);
			sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Device request to barge exten '%s'\n", d->id, shortenedNumber);
				if (sccp_feat_barge(c, shortenedNumber)) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				}
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
			}
			d = sccp_device_release(d);
			return NULL;										// leave simpleswitch without dial
		case SCCP_SS_GETCBARGEROOM:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Conference Barge Extension\n", d->id);
			// like we're dialing but we're not :)
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_channel_send_callinfo(d, c);
			sccp_dev_clearprompt(d, instance, c->callid);
			sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Device request to barge conference '%s'\n", d->id, shortenedNumber);
				if (sccp_feat_cbarge(c, shortenedNumber)) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				}
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
			}
			d = sccp_device_release(d);
			return NULL;										// leave simpleswitch without dial
		case SCCP_SS_DIAL:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Dial Extension\n", d->id);
		default:
			break;
	}

	/* set private variable */
	if (chan && !pbx_check_hangup(chan)) {
		sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: set variable SKINNY_PRIVATE to: %s\n", c->privacy ? "1" : "0");
		if (c->privacy) {

			//chan->cid.cid_pres = AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED;
			sccp_channel_set_calleridPresenceParameter(c, CALLERID_PRESENCE_FORBIDDEN);
		}

		uint32_t result = d->privacyFeature.status & SCCP_PRIVACYFEATURE_CALLPRESENT;

		result |= c->privacy;
		if (d->privacyFeature.enabled && result) {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: set variable SKINNY_PRIVATE to: %s\n", "1");
			pbx_builtin_setvar_helper(chan, "SKINNY_PRIVATE", "1");
		} else {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: set variable SKINNY_PRIVATE to: %s\n", "0");
			pbx_builtin_setvar_helper(chan, "SKINNY_PRIVATE", "0");
		}
	}

	/* set devicevariables */
	v = ((d) ? d->variables : NULL);
	while (chan && !pbx_check_hangup(chan) && d && v) {
		pbx_builtin_setvar_helper(chan, v->name, v->value);
		v = v->next;
	}

	/* set linevariables */
	v = ((l) ? l->variables : NULL);
	while (chan && !pbx_check_hangup(chan) && l && v) {
		pbx_builtin_setvar_helper(chan, v->name, v->value);
		v = v->next;
	}

//      sccp_copy_string(chan->exten, shortenedNumber, sizeof(chan->exten));
	PBX(setChannelExten) (c, shortenedNumber);
	sccp_copy_string(d->lastNumber, c->dialedNumber, sizeof(d->lastNumber));

	sccp_softkey_setSoftkeyState(d, KEYMODE_ONHOOK, SKINNY_LBL_REDIAL, TRUE); /** enable redial key */
	sccp_channel_set_calledparty(c, c->dialedNumber, shortenedNumber);

	/* The 7961 seems to need the dialing callstate to record its directories information. */
	sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);

	/* proceed call state is needed to display the called number.
	   The phone will not display callinfo in offhook state */
	sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	sccp_channel_send_callinfo(d, c);

	sccp_dev_clearprompt(d, instance, c->callid);
	sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);

	if (!sccp_strlen_zero(shortenedNumber) && !pbx_check_hangup(chan)
	    && pbx_exists_extension(chan, pbx_channel_context(chan), shortenedNumber, 1, l->cid_num)) {
		/* found an extension, let's dial it */
		sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x is dialing number %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
		/* Answer dialplan command works only when in RINGING OR RING ast_state */
		PBX(set_callstate) (c, AST_STATE_RING);

		int8_t pbxStartResult = pbx_pbx_start(chan);

		/* \todo replace AST_PBX enum using pbx_impl wrapper enum */
		switch (pbxStartResult) {
			case AST_PBX_FAILED:
				pbx_log(LOG_ERROR, "%s: (sccp_pbx_softswitch) channel %s-%08x failed to start new thread to dial %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
				/* \todo change indicate to something more suitable */
				sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				break;
			case AST_PBX_CALL_LIMIT:
				pbx_log(LOG_WARNING, "%s: (sccp_pbx_softswitch) call limit reached for channel %s-%08x failed to start new thread to dial %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
				sccp_indicate(d, c, SCCP_CHANNELSTATE_CONGESTION);
				break;
			default:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) pbx started\n", DEV_ID_LOG(d));
#        ifdef CS_MANAGER_EVENTS
				if (GLOB(callevents)) {
					manager_event(EVENT_FLAG_SYSTEM, "ChannelUpdate", "Channel: %s\r\nUniqueid: %s\r\nChanneltype: %s\r\nSCCPdevice: %s\r\nSCCPline: %s\r\nSCCPcallid: %s\r\n", PBX(getChannelName) (c), PBX(getChannelUniqueID) (c), "SCCP", (d) ? DEV_ID_LOG(d) : "(null)", (l && l->name) ? l->name : "(null)", (c && c->callid) ? (char *)&c->callid : "(null)");
				}
#        endif
				break;
		}
	} else {

		sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x shortenedNumber: %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
		sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x pbx_check_hangup(chan): %d\n", DEV_ID_LOG(d), l->name, c->callid, pbx_check_hangup(chan));
		sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x extension exists: %s\n", DEV_ID_LOG(d), l->name, c->callid, pbx_exists_extension(chan, pbx_channel_context(chan), shortenedNumber, 1, l->cid_num) ? "TRUE" : "FALSE");
		/* timeout and no extension match */
		sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
	}

	sccp_log((DEBUGCAT_PBX | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) quit\n", DEV_ID_LOG(d));

	d = sccp_device_release(d);
	return NULL;
#    else
	if (!c) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <channel> available. Returning from dial thread.\n");
		return NULL;
	}
	sccp_device_t *d = NULL;

	/* Reset Enbloc Dial Emulation */
	c->enbloc.deactivate = 0;
	c->enbloc.totaldigittime = 0;
	c->enbloc.totaldigittimesquared = 0;
	c->enbloc.digittimeout = GLOB(digittimeout) * 1000;

	/* prevent softswitch from being executed twice (Pavel Troller / 15-Oct-2010) */
	if (PBX(getChannelPbx) (c)) {
		sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: (sccp_pbx_softswitch) PBX structure already exists. Dialing instead of starting.\n");
		/* If there are any digits, send them instead of starting the PBX */
		if (!sccp_strlen_zero(c->dialedNumber)) {
			sccp_pbx_senddigits(c, c->dialedNumber);
			sccp_channel_set_calledparty(c, c->dialedNumber, c->dialedNumber);
			if ((d = sccp_channel_getDevice_retained(c))) {
				sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
				d = sccp_device_release(d);
			}
		}
		return NULL;
	}

	PBX_CHANNEL_TYPE *chan = c->owner;

	PBX_VARIABLE_TYPE *v = NULL;

	uint8_t instance;

	unsigned int len = 0;

	sccp_line_t *l;

	char shortenedNumber[256] = { '\0' };									/* For recording the digittimeoutchar */

	/* removing scheduled dialing */
	c->scheduler.digittimeout = SCCP_SCHED_DEL(c->scheduler.digittimeout);

	/* we should just process outbound calls, let's check calltype */
	if (c->calltype != SKINNY_CALLTYPE_OUTBOUND) {
		d = sccp_device_release(d);
		return NULL;
	}

	/* assume d is the channel's device */
	/* does it exists ? */
	if (!(d = sccp_channel_getDevice_retained(c))) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <device> available. Returning from dial thread.\n");
		return NULL;
	}

	/* we don't need to check for a device type but just if the device has an id, otherwise back home  -FS */
	if (!d->id || sccp_strlen_zero(d->id)) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <device> identifier available. Returning from dial thread.\n");
		d = sccp_device_release(d);
		return NULL;
	}

	l = c->line;
	if (!l) {
		pbx_log(LOG_ERROR, "SCCP: (sccp_pbx_softswitch) No <line> available. Returning from dial thread.\n");
		if (chan)
			PBX(requestHangup) (chan);
		d = sccp_device_release(d);
		return NULL;
	}

	instance = sccp_device_find_index_for_line(d, c->line->name);
	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) New call on line %s\n", DEV_ID_LOG(d), l->name);

	/* assign callerid name and number */
	//sccp_channel_set_callingparty(c, l->cid_name, l->cid_num);

	// we use shortenedNumber but why ???
	// If the timeout digit has been used to terminate the number
	// and this digit shall be included in the phone call history etc (recorddigittimeoutchar is true)
	// we still need to dial the number without the timeout char in the pbx
	// so that we don't dial strange extensions with a trailing characters.
	sccp_copy_string(shortenedNumber, c->dialedNumber, sizeof(shortenedNumber));
	len = strlen(shortenedNumber);
	assert(strlen(c->dialedNumber) == len);

	if (len > 0 && GLOB(digittimeoutchar) == shortenedNumber[len - 1]) {
		shortenedNumber[len - 1] = '\0';

		// If we don't record the timeoutchar in the logs, we remove it from the sccp channel structure
		// Later, the channel dialed number is used for directories, etc.,
		// and the shortened number is used for dialing the actual call via asterisk pbx.
		if (!GLOB(recorddigittimeoutchar)) {
			c->dialedNumber[len - 1] = '\0';
		}
	}

	/* This will choose what to do */
	switch (c->ss_action) {
		case SCCP_SS_GETFORWARDEXTEN:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Forward Extension\n", d->id);
			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_line_cfwd(l, d, c->ss_data, shortenedNumber);
			}
			sccp_channel_endcall(c);
			d = sccp_device_release(d);
			return NULL;										// leave simple switch without dial
#        ifdef CS_SCCP_PICKUP
		case SCCP_SS_GETPICKUPEXTEN:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Pickup Extension\n", d->id);
			// like we're dialing but we're not :)
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_channel_send_callinfo(d, c);
			sccp_dev_clearprompt(d, instance, c->callid);
			sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);

			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "SCCP: Asterisk request to pickup exten '%s'\n", shortenedNumber);
				if (sccp_feat_directpickup(c, shortenedNumber)) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				}
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
			}
			d = sccp_device_release(d);
			return NULL;										// leave simpleswitch without dial
#        endif									// CS_SCCP_PICKUP
		case SCCP_SS_GETMEETMEROOM:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme request\n", d->id);
			if (!sccp_strlen_zero(shortenedNumber) && !sccp_strlen_zero(c->line->meetmenum)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme request for room '%s' on extension '%s'\n", d->id, shortenedNumber, c->line->meetmenum);
				if (c->owner && !pbx_check_hangup(c->owner))
					pbx_builtin_setvar_helper(c->owner, "SCCP_MEETME_ROOM", shortenedNumber);
				sccp_copy_string(shortenedNumber, c->line->meetmenum, sizeof(shortenedNumber));

				//sccp_copy_string(c->dialedNumber, SKINNY_DISP_CONFERENCE, sizeof(c->dialedNumber));
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Start Meetme Thread\n", d->id);
				sccp_feat_meetme_start(c);							/* Copied from Federico Santulli */
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Meetme Thread Started\n", d->id);
				d = sccp_device_release(d);
				return NULL;
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
				d = sccp_device_release(d);
				return NULL;
			}
			break;
		case SCCP_SS_GETBARGEEXTEN:
			// like we're dialing but we're not :)
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Barge Extension\n", d->id);
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_channel_send_callinfo(d, c);

			sccp_dev_clearprompt(d, instance, c->callid);
			sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Device request to barge exten '%s'\n", d->id, shortenedNumber);
				if (sccp_feat_barge(c, shortenedNumber)) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				}
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
			}
			d = sccp_device_release(d);
			return NULL;										// leave simpleswitch without dial
		case SCCP_SS_GETCBARGEROOM:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Get Conference Barge Extension\n", d->id);
			// like we're dialing but we're not :)
			sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);
			sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
			sccp_channel_send_callinfo(d, c);
			sccp_dev_clearprompt(d, instance, c->callid);
			sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
			if (!sccp_strlen_zero(shortenedNumber)) {
				sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Device request to barge conference '%s'\n", d->id, shortenedNumber);
				if (sccp_feat_cbarge(c, shortenedNumber)) {
					sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				}
			} else {
				// without a number we can also close the call. Isn't it true ?
				sccp_channel_endcall(c);
			}
			d = sccp_device_release(d);
			return NULL;										// leave simpleswitch without dial
		case SCCP_SS_DIAL:
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "%s: (sccp_pbx_softswitch) Dial Extension\n", d->id);
		default:
			break;
	}

	/* set private variable */
	if (chan && !pbx_check_hangup(chan)) {
		sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: set variable SKINNY_PRIVATE to: %s\n", c->privacy ? "1" : "0");
		if (c->privacy) {

			//chan->cid.cid_pres = AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED;
			sccp_channel_set_calleridPresenceParameter(c, CALLERID_PRESENCE_FORBIDDEN);
		}

		uint32_t result = d->privacyFeature.status & SCCP_PRIVACYFEATURE_CALLPRESENT;

		result |= c->privacy;
		if (d->privacyFeature.enabled && result) {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: set variable SKINNY_PRIVATE to: %s\n", "1");
			pbx_builtin_setvar_helper(chan, "SKINNY_PRIVATE", "1");
		} else {
			sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_3 "SCCP: set variable SKINNY_PRIVATE to: %s\n", "0");
			pbx_builtin_setvar_helper(chan, "SKINNY_PRIVATE", "0");
		}
	}

	/* set devicevariables */
	v = ((d) ? d->variables : NULL);
	while (chan && !pbx_check_hangup(chan) && d && v) {
		pbx_builtin_setvar_helper(chan, v->name, v->value);
		v = v->next;
	}

	/* set linevariables */
	v = ((l) ? l->variables : NULL);
	while (chan && !pbx_check_hangup(chan) && l && v) {
		pbx_builtin_setvar_helper(chan, v->name, v->value);
		v = v->next;
	}

	PBX(setChannelExten) (c, shortenedNumber);
	sccp_copy_string(d->lastNumber, c->dialedNumber, sizeof(d->lastNumber));

	sccp_softkey_setSoftkeyState(d, KEYMODE_ONHOOK, SKINNY_LBL_REDIAL, TRUE); /** enable redial key */
	sccp_channel_set_calledparty(c, c->dialedNumber, shortenedNumber);

	/* The 7961 seems to need the dialing callstate to record its directories information. */
	sccp_indicate(d, c, SCCP_CHANNELSTATE_DIALING);

	/* proceed call state is needed to display the called number.
	   The phone will not display callinfo in offhook state */
	sccp_device_sendcallstate(d, instance, c->callid, SKINNY_CALLSTATE_PROCEED, SKINNY_CALLPRIORITY_LOW, SKINNY_CALLINFO_VISIBILITY_DEFAULT);
	sccp_channel_send_callinfo(d, c);

	sccp_dev_clearprompt(d, instance, c->callid);
	sccp_dev_displayprompt(d, instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);

	if (!sccp_strlen_zero(shortenedNumber) && !pbx_check_hangup(chan)
	    && pbx_exists_extension(chan, pbx_channel_context(chan), shortenedNumber, 1, l->cid_num)) {
		/* found an extension, let's dial it */
		sccp_log((DEBUGCAT_PBX | DEBUGCAT_CHANNEL)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x is dialing number %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
		/* Answer dialplan command works only when in RINGING OR RING ast_state */
		PBX(set_callstate) (c, AST_STATE_RING);

		int8_t pbxStartResult = pbx_pbx_start(chan);

		/* \todo replace AST_PBX enum using pbx_impl wrapper enum */
		switch (pbxStartResult) {
			case AST_PBX_FAILED:
				pbx_log(LOG_ERROR, "%s: (sccp_pbx_softswitch) channel %s-%08x failed to start new thread to dial %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
				/* \todo change indicate to something more suitable */
				sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
				break;
			case AST_PBX_CALL_LIMIT:
				pbx_log(LOG_WARNING, "%s: (sccp_pbx_softswitch) call limit reached for channel %s-%08x failed to start new thread to dial %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
				sccp_indicate(d, c, SCCP_CHANNELSTATE_CONGESTION);
				break;
			default:
				sccp_log((DEBUGCAT_PBX)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) pbx started\n", DEV_ID_LOG(d));
#        ifdef CS_MANAGER_EVENTS
				if (GLOB(callevents)) {
					manager_event(EVENT_FLAG_SYSTEM, "ChannelUpdate", "Channel: %s\r\nUniqueid: %s\r\nChanneltype: %s\r\nSCCPdevice: %s\r\nSCCPline: %s\r\nSCCPcallid: %s\r\n", (chan) ? pbx_channel_name(chan) : "(null)", (chan) ? pbx_channel_uniqueid(chan) : "(null)", "SCCP", (d) ? DEV_ID_LOG(d) : "(null)", (l && l->name) ? l->name : "(null)", (c && c->callid) ? (char *)&c->callid : "(null)");
				}
#        endif
				break;
		}
	} else {

		sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x shortenedNumber: %s\n", DEV_ID_LOG(d), l->name, c->callid, shortenedNumber);
		sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x pbx_check_hangup(chan): %d\n", DEV_ID_LOG(d), l->name, c->callid, pbx_check_hangup(chan));
		sccp_log(DEBUGCAT_PBX) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) channel %s-%08x extension exists: %s\n", DEV_ID_LOG(d), l->name, c->callid, pbx_exists_extension(chan, pbx_channel_context(chan), shortenedNumber, 1, l->cid_num) ? "TRUE" : "FALSE");
		/* timeout and no extension match */
		sccp_indicate(d, c, SCCP_CHANNELSTATE_INVALIDNUMBER);
	}

	sccp_log((DEBUGCAT_PBX | DEBUGCAT_DEVICE)) (VERBOSE_PREFIX_1 "%s: (sccp_pbx_softswitch) quit\n", DEV_ID_LOG(d));

	d = sccp_device_release(d);
	return NULL;
#    endif
}

/*!
 * \brief Send Digit to Asterisk
 * \param c SCCP Channel
 * \param digit Digit as char
 */
void sccp_pbx_senddigit(sccp_channel_t * c, char digit)
{
	if (PBX(send_digit))
		PBX(send_digit) (c, digit);
}

/*!
 * \brief Send Multiple Digits to Asterisk
 * \param c SCCP Channel
 * \param digits Multiple Digits as char
 */
void sccp_pbx_senddigits(sccp_channel_t * c, const char *digits)
{
	if (PBX(send_digits))
		PBX(send_digits) (c, digits);
}

/*!
 * \brief Handle Dialplan Transfer
 *
 * This will allow asterisk to transfer an SCCP Channel via the dialplan transfer function
 *
 * \param ast Asterisk Channel
 * \param dest Destination as char *
 * \return result as int
 *
 * \test Dialplan Transfer Needs to be tested
 * \todo pbx_transfer needs to be implemented correctly
 * 
 * \called_from_asterisk
 */
int sccp_pbx_transfer(PBX_CHANNEL_TYPE * ast, const char *dest)
{
	int res = 0;

	sccp_channel_t *c;

	if (dest == NULL) {											/* functions below do not take a NULL */
		dest = "";
		return -1;
	}

	c = get_sccp_channel_from_pbx_channel(ast);
	if (!c) {
		return -1;
	}

/*
        sccp_device_t *d;
        sccp_channel_t *newcall;
*/

	sccp_log((DEBUGCAT_CORE)) (VERBOSE_PREFIX_1 "Transferring '%s' to '%s'\n", PBX(getChannelName) (c), dest);
	if (pbx_channel_state(ast) == AST_STATE_RING) {
		//! \todo Blindtransfer needs to be implemented correctly

/*
		res = sccp_blindxfer(p, dest);
*/
		res = -1;
	} else {
		//! \todo Transfer needs to be implemented correctly

/*
		res=sccp_channel_transfer(p,dest);
*/
		res = -1;
	}
	c = sccp_channel_release(c);
	return res;
}

/*
 * \brief ACF Channel Read callback
 *
 * \param ast Asterisk Channel
 * \param funcname	functionname as const char *
 * \param args		arguments as char *
 * \param buf		buffer as char *
 * \param buflen 	bufferlenght as size_t
 * \return result as int
 *
 * \called_from_asterisk
 * 
 * \test ACF Channel Read Needs to be tested
 */
// int acf_channel_read(PBX_CHANNEL_TYPE *ast, NEWCONST char *funcname, char *args, char *buf, size_t buflen);
// 
// int acf_channel_read(PBX_CHANNEL_TYPE *ast, NEWCONST char *funcname, char *args, char *buf, size_t buflen)
// {
//      sccp_channel_t *c;
// 
//      c = get_sccp_channel_from_pbx_channel(ast);
//      if (c == NULL)
//              return -1;
// 
//      if (!strcasecmp(args, "peerip")) {
//              sccp_copy_string(buf, c->rtp.audio.phone_remote.sin_addr.s_addr ? pbx_inet_ntoa(c->rtp.audio.phone_remote.sin_addr) : "", buflen);
//      } else if (!strcasecmp(args, "recvip")) {
//              sccp_copy_string(buf, c->rtp.audio.phone.sin_addr.s_addr ? pbx_inet_ntoa(c->rtp.audio.phone.sin_addr) : "", buflen);
//      } else if (!strcasecmp(args, "from") && c->device) {
//              sccp_copy_string(buf, (char *)c->device->id, buflen);
//      } else {
//              c = sccp_channel_release(c);
//              return -1;
//      }
//      c = sccp_channel_release(c);
//      return 0;
// }

/*!
 * \brief Get (remote) peer for this channel
 * \param channel 	SCCP Channel
 *
 * \deprecated
 */

/* 
sccp_channel_t *sccp_pbx_getPeer(sccp_channel_t * channel)
{
	//! \todo implement internal peer search
	return NULL;
}
*/

/*!
 * \brief Get codec capabilities for local channel
 * \param channel 	SCCP Channel
 * \param capabilities 	Codec Capabilities
 *
 * \deprecated
 * \note Variable capabilities will be malloced by function, caller must destroy this later
 */

/*
int sccp_pbx_getCodecCapabilities(sccp_channel_t * channel, void **capabilities)
{
	//! \todo implement internal peer search
	return -1;
}
*/

/**
 * \brief Get Peer Codec Capabilies
 * \param channel 	SCCP Channel
 * \param capabilities 	Codec Capabilities
 *
 * \note Variable capabilities will be malloced by function, caller must destroy this later
 */

/*
int sccp_pbx_getPeerCodecCapabilities(sccp_channel_t * channel, void **capabilities)
{
//	sccp_channel_t *peer;

	PBX(getPeerCodecCapabilities) (channel, capabilities);
	return 1;
}
*/
#endif

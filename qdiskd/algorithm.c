/*
 * Copyright (c) 2025 IBM.
 *
 * All rights reserved.
 *
 * Author: Thomas Jones (thomas.jones@ibm.com)
 *         Michael Baker
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
 * - Neither the name of the Red Hat, Inc. nor the names of its
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

#include "algorithm.h"
#include "persistent_reserve/persistent_reserve.h"
#include "cmap.h"
#include "log.h"
#include "vquorum.h"

#include <limits.h>
#include <stdbool.h>

/// Get a monotonic time in miliseconds
static uint64_t gettime_ms(void);

/// Check that all nodes we can see are registered to the same disk as us
static pr_dev_err validate_node_quorum_disk_keys(uint32_t num_node_keys, uint64_t *node_keys);

static const char *get_algorithm_state_name(int s);

static struct persistent_reserve_device *pr_device = NULL;
static uint32_t algorithm_heartbeat = 0;
static uint32_t algorithm_timeout = 0;

static enum state { START, SINGLE_NODE, IDLE, RESERVE_DISK, RECV_VOTE, HAVE_QDISK, RELEASE_QDISK, RESIGNED, QDISK_NUM_STATES_ } state = START;

pr_dev_err algorithm_init(void)
{
	char *device = NULL;
	char *key_file = NULL;
	char *reserve_key_str = NULL;
	pr_dev_err err = PR_ERR_OK;
	cs_error_t cs_err = CS_OK;
	uint64_t ikey = 0;
	bool registered_key_with_disk = false;

	if(CS_OK != qdisk_cmap_get_qdisk_device(&device)) {
		ENGN_LOG(LOG_ERR, "Failed to get device path.  Verify the configuration in corosync.conf");
		return PR_ERR_DEVICE_NOT_OPERABLE;
	}

	qdisk_cmap_get_heartbeat(&algorithm_heartbeat);
	qdisk_cmap_get_timeout(&algorithm_timeout);
	ENGN_LOG(LOG_NOTICE, "QDisk device set to: '%s'\n", device);
	ENGN_LOG(LOG_NOTICE, "Algorithm heartbeat set to: %ums\n", algorithm_heartbeat);
	ENGN_LOG(LOG_NOTICE, "Algorithm timeout set to: %ums\n", algorithm_timeout);

	if(!is_scsi_device(device) && !is_nvme_device(device)) {
		ENGN_LOG(LOG_ERR, "The disk is neither SCSI nor NVME. Device detection failed");
		free(device);
		return PR_ERR_DEVICE_INVALID_DEVICE_TYPE;
	}

	if(CS_OK == qdisk_cmap_get_qdisk_keystr(&reserve_key_str)) {
		int scanf_res = sscanf(reserve_key_str, "%" PERSISTENT_RESERVE_PRI_KEY, &ikey);
		free(reserve_key_str);
		reserve_key_str = NULL;
		if(scanf_res != 1) {
			ENGN_LOG(LOG_ERR, "QDisk key string has bad format. Verify the configuration in corosync.conf");
			return PR_ERR_DEVICE_NOT_OPERABLE;
		}
	} else if(CS_OK != qdisk_cmap_get_qdisk_key_file(&key_file)) {
		key_file = strdup(PERSISTENT_RESERVE_DEFAULT_KEYFILE);
	}

	if(key_file) {
		ENGN_LOG(LOG_NOTICE, "Registration Keyfile set to: %s\n", key_file);
		pr_device = persistent_reserve_device_new(device, key_file);
		free(key_file);
		key_file = NULL;
	} else {
		pr_device = persistent_reserve_device_new_key(device, ikey);
	}
	free(device);
	if(NULL == pr_device) {
		ENGN_LOG(LOG_ERR, "Could not allocate memory for device handle");
		return PR_ERR_DEVICE_MEMORY_ALLOCATION_FAILED;
	}

	ENGN_LOG(LOG_NOTICE, "Registration Key set to: %" PERSISTENT_RESERVE_PRI_KEY "\n",
	         persistent_reserve_device_get_key(pr_device));

	do {
		uint32_t num_node_keys = 0;
		uint64_t *node_keys = NULL;
		if(CS_OK != vquorum_get_node_key_list(&num_node_keys, &node_keys)) {
			ENGN_LOG(LOG_ERR, "Failed to fetch keys from votequorum (%s)", cs_strerror(cs_err));
			err = PR_ERR_UNKNOWN;
			goto err_exit;
		}

		err = validate_node_quorum_disk_keys(num_node_keys, node_keys);
		if(PR_ERR_OK != err) {
			goto err_exit;
		}
	} while(0);

	// If we hold the reservation we should release it since we aren't yet providing service
	if(0 < persistent_reserve_device_is_reserved(pr_device)) {
		uint64_t reservation_holder = 0;
		err = persistent_reserve_device_reservation_owner_key(pr_device, &reservation_holder);
		if(PR_ERR_OK == err && reservation_holder == persistent_reserve_device_get_key(pr_device)) {
			persistent_reserve_device_release(pr_device);
		}
		if(PR_ERR_OK == err && reservation_holder == 0) {
			ENGN_LOG(LOG_CRIT, "Key 0 holding a disk reservation!");
			qdisk_dump_blackbox();
			err = PR_ERR_DEVICE_NOT_OPERABLE;
			goto err_exit;
		}
	}

	cs_err = vquorum_qdevice_register();
	if(CS_OK != cs_err) {
		ENGN_LOG(LOG_ERR, "Failed to register with votequorum. (%s)", cs_strerror(cs_err));
		err = PR_ERR_UNKNOWN;
		goto err_exit;
	}

	err = persistent_reserve_device_register_key(pr_device);
	if( PR_ERR_OK != err) {
		ENGN_LOG(LOG_ERR, "Failed to register our key.");
		goto err_exit;
	}
	registered_key_with_disk = true;
	ENGN_LOG(LOG_NOTICE, "Registered with disk...\n");

	vquorum_qdisk_set_qdisk_key(persistent_reserve_device_get_key(pr_device));
	cs_err = vquorum_qdisk_share_ikey(persistent_reserve_device_get_key(pr_device));
	if(CS_OK != cs_err) {
		ENGN_LOG(LOG_ERR, "Failed to share our key. (%s)", cs_strerror(cs_err));
		err = PR_ERR_UNKNOWN;
		goto err_exit;
	}

	ENGN_LOG(LOG_NOTICE, "Shared key with cluster...\n");

	return PR_ERR_OK;

err_exit:
	if(pr_device) {
		if(registered_key_with_disk) {
			persistent_reserve_device_unregister_key(pr_device);
		}
		persistent_reserve_device_free(pr_device);
		pr_device = NULL;
	}

	vquorum_qdevice_unregister();

	return err;
}

/** Run one heartbeat, iterate state machine
 */
void algorithm_run(void)
{
	uint32_t node_count = 0;
	uint32_t num_node_keys = 0;
	uint64_t *node_keys = NULL;
	uint64_t reservation_holder = 0;
	int reserved = 0;
	bool resv_node_visible = false;
	bool is_quorate = false;
	uint32_t num_nodes_visible = 0;
	uint32_t votes = 0;
	uint32_t expected_votes = 0;
	uint32_t *nodelist = NULL;
	int cast_vote = 0;
	bool skip_poll = false;
	int prev_state = state;
	pr_dev_err pr_err = PR_ERR_OK;
	cs_error_t cs_err = CS_OK;

	static uint64_t timeout_time = 0;
	static uint64_t good_heartbeat_timeout = UINT64_MAX;

	if(good_heartbeat_timeout < gettime_ms()) {
		ENGN_LOG(LOG_CRIT, "Too many skipped heartbeats in a row!");
		qdisk_dump_blackbox();
		algorithm_stop();
		exit(1); // Want to put the unit in the failed state
	}

	cs_err = qdisk_cmap_get_node_count(&node_count);
	if(CS_OK != cs_err) {
		ENGN_LOG(LOG_ERR, "Failed to get node count from cmap (%s), skipping this heartbeat.", cs_strerror(cs_err));
		return;
	}

	// node_count = vquorum_get_node_count();
	is_quorate = vquorum_get_quorate();
	expected_votes = vquorum_get_expected_votes();
	votes = vquorum_get_total_votes();
	reserved = persistent_reserve_device_is_reserved(pr_device);

	if(reserved < 0) {
		ENGN_LOG(LOG_ERR, "Failed to check device reservation status, skipping this heartbeat.");
		return;
	}

	if (reserved) {
		pr_err = persistent_reserve_device_reservation_owner_key(pr_device, &reservation_holder);
		if(PR_ERR_OK != pr_err) {
			ENGN_LOG(LOG_ERR, "Failed to fetch reservation owning key, skipping this heartbeat.");
			return;
		}

		if(reservation_holder == 0) {
			ENGN_LOG(LOG_CRIT, "Key 0 holding a disk reservation!");
			qdisk_dump_blackbox();
			algorithm_stop();
			exit(1); // Want to put the unit in the failed state
		}

		// if somehow we hold the reservation when we shouldn't, release it
		if(state != HAVE_QDISK && state != RELEASE_QDISK && state != RESERVE_DISK
			&& reservation_holder == persistent_reserve_device_get_key(pr_device))
		{
			ENGN_LOG(LOG_ERR, "Unexptectedly holding device reservation, releasing and skipping this heartbeat");
			persistent_reserve_device_release(pr_device);
			return;
		}
	}

	if(CS_OK != vquorum_get_node_key_list(&num_node_keys, &node_keys)) {
		ENGN_LOG(LOG_ERR, "Failed to fetch keys from votequorum, skipping this heartbeat.");
		return;
	}

	vquorum_get_node_list(&num_nodes_visible, &nodelist); // doesn't need freeing, no allocation

	// Check if we can see the node holding the reservation
	for(uint32_t i=0; i < num_node_keys && !resv_node_visible; i++) {
		if(node_keys[i]) {
			resv_node_visible = (node_keys[i] == reservation_holder);
		}
	}

	// Validate that we see our own key on the disk, should always unless something strange is going on
	do {
		size_t num_registered_keys = 0;
		uint64_t *registered_keys = NULL;
		if(PR_ERR_OK != persistent_reserve_device_get_registered_keys(pr_device, &num_registered_keys, &registered_keys)) {
			ENGN_LOG(LOG_ERR, "Failed to fetch current registered keys, skipping this heartbeat.");
			free(node_keys);
			return;
		}

		bool found = false;
		for(size_t j=0; j < num_registered_keys && !found; j++) {
			found = (persistent_reserve_device_get_key(pr_device) == registered_keys[j]);
		}

		free(registered_keys);
		registered_keys = NULL;

		if(!found) {
			ENGN_LOG(LOG_ERR, "Our reservation key %" PERSISTENT_RESERVE_PRI_KEY " is no longer registered on the disk", 
			         persistent_reserve_device_get_key(pr_device));
			qdisk_dump_blackbox();
			algorithm_stop();
			exit(1); // Will put our systemd unit into failed state rather than restarting us
			// Want to fail so we can potentially fence by expelling keys
		}
	} while(0);

	ENGN_LOG(LOG_DEBUG, "Quorate:%d State: %s Votes: %d (Expected %d) Nodes: %d (%d visible) Reserved:%d (Key=%" PERSISTENT_RESERVE_PRI_KEY ") RFlag:%d",
	         is_quorate, get_algorithm_state_name(state), votes, expected_votes, node_count, num_nodes_visible, reserved, reservation_holder, resv_node_visible);

	do {
		char buf[4096] = { 0 };
		size_t pos = 0;
		for(uint32_t i=0; i < num_node_keys && pos+1 < sizeof(buf); i++) {
			int r = snprintf(buf+pos, sizeof(buf)-pos-1, "%" PERSISTENT_RESERVE_PRI_KEY "\n", node_keys[i]);
			if(r > 0) {
				pos += r;
			}
		}
		ENGN_LOG(LOG_TRACE, "Visible keys:\n%s\n", buf);
	} while(0);

	bool tiebreak_needed = num_nodes_visible*2 == node_count;

	switch(state) {
	case START:
		cast_vote = 0;
		// if(PR_ERR_FOUND_UNREGISTERED_NODE_KEY == validate_node_keys(num_node_keys, node_keys)) { // Check for disk misconfiguration
		// 	abort(); // validate_node_keys() will log a message
		// }

		if(is_quorate) {
			ENGN_LOG(LOG_NOTICE, "Cluster Quorate, ready to serve");
			state = IDLE;
		} else if(node_count == 1 && num_nodes_visible == 1) {
			/* Special case: single-node cluster. The node cannot reach quorum
			 * until qdisk provides its vote, creating a deadlock. Cast the
			 * vote now while still in START so corosync can become quorate,
			 * and we will transition to IDLE on the next heartbeat once quorate. */
			ENGN_LOG(LOG_NOTICE, "Single-node cluster detected, providing vote to bootstrap quorum");
			cast_vote = 1;
            state = SINGLE_NODE;
		}
		break;

    case SINGLE_NODE:
        cast_vote = 1;
        if(node_count >= 1) {
            state = IDLE;
        }
        break;

	case IDLE:
		cast_vote = 1;
		if(tiebreak_needed) {
			if(resv_node_visible) {
				timeout_time = gettime_ms() + algorithm_timeout;
				state = RECV_VOTE;
			} else if(!reserved) {
				timeout_time = gettime_ms() + algorithm_timeout;
				state = RESERVE_DISK;
			} else if(gettime_ms() > timeout_time) {
				cast_vote = 0;
				state = RESIGNED;
			} else {
				// We're in a tiebreak but don't know if we're in the winning half, skip sending a poll
				skip_poll = true;
			}
		} else if(num_nodes_visible*2 < node_count) {
			cast_vote = 0;
			state = RESIGNED;
		} else if( 1 >= node_count ) {
			cast_vote = 1;
			state = SINGLE_NODE;
		} else {
			timeout_time = gettime_ms() + algorithm_timeout;
		}
		// if(PR_ERR_FOUND_UNREGISTERED_NODE_KEY == validate_node_keys(num_node_keys, node_keys)) {
		// 	abort(); // validate_node_keys() will log a message
		// }
		break;

	case RESERVE_DISK:
		cast_vote = 1;
		pr_err = persistent_reserve_device_reserve(pr_device);
		if(PR_ERR_OK == pr_err) {
			ENGN_LOG(LOG_NOTICE, "Successfullly made disk reservation on %s",
			         persistent_reserve_device_get_name(pr_device));
			state = HAVE_QDISK;
		} else if(resv_node_visible) {
			state = RECV_VOTE;
		} else if(reserved && !resv_node_visible) { // full condition for clarity
			ENGN_LOG(LOG_NOTICE, "Disk is reserved and can't see reservation holder, giving up on obtaining reservation.");
			state = RESIGNED;
			cast_vote = 0;
		} else if(gettime_ms() > timeout_time) {
			ENGN_LOG(LOG_NOTICE, "Timeout after %" PRIu32 "ms. Can't see reservation holder, giving up on obtaining reservation.", algorithm_timeout);
			state = RESIGNED;
			cast_vote = 0;
		} else {
			skip_poll = true;
			ENGN_LOG(LOG_NOTICE, "Failed disk reservation on %s, will retry",
			         persistent_reserve_device_get_name(pr_device));
		}
		break;

	case RECV_VOTE:
		cast_vote = 1;
		if(resv_node_visible && tiebreak_needed) {
			timeout_time = gettime_ms() + algorithm_timeout;
		} else if(gettime_ms() > timeout_time) {
			ENGN_LOG(LOG_INFO, "Returning to idle after %" PRIu32 "ms", algorithm_timeout);
			state = IDLE;
		}
		break;

	case HAVE_QDISK:
		cast_vote = 1;
		if(!reserved || reservation_holder != persistent_reserve_device_get_key(pr_device)) {
			ENGN_LOG(LOG_ERR, "Lost disk reservation on %s.",
			         persistent_reserve_device_get_name(pr_device));
			abort();
		}
		if(!tiebreak_needed) {
			state = RELEASE_QDISK;
		}
		break;

	case RELEASE_QDISK:
		cast_vote = 1;
		// loop until the reservation is not held
		if(PR_ERR_OK == persistent_reserve_device_release(pr_device) || 0 == persistent_reserve_device_is_reserved(pr_device)) {
			state = IDLE;
		}
		break;

	case RESIGNED:
		cast_vote = 0;
		if(is_quorate) { // either no longer tiebreaker state or we somehow joined the winning partition
			ENGN_LOG(LOG_NOTICE, "Cluster quorate. Returning to idle");
			state = IDLE;
		}
		break;

	case QDISK_NUM_STATES_:
	default:
		ENGN_LOG(LOG_EMERG, "Daemon in unknown state!");
		abort();
	}

	static bool old_skip_poll = false;
	static bool old_quorate = false;
	static uint32_t old_node_count = 0;
	static uint32_t old_votes = 0;
	static uint32_t old_expected_votes = 0;
	static uint32_t old_num_nodes_visible = 0;
	static bool old_reserved = false;
	static bool old_resv_node_visible = false;
	static int old_cast_vote = 0;

	if(old_skip_poll != skip_poll || prev_state != state || old_quorate != is_quorate || old_votes != votes || old_expected_votes != expected_votes
	   || old_node_count != node_count || old_num_nodes_visible != num_nodes_visible
	   || old_reserved != reserved || old_resv_node_visible != resv_node_visible || old_cast_vote != cast_vote)
	{
		ENGN_LOG(LOG_INFO, "State:%s->%s SkipPoll:%d Quorate:%d Votes: %d (Expected %d) Nodes: %d (%d visible) Reserved:%d (Key=%" PERSISTENT_RESERVE_PRI_KEY ") RFlag:%d CastVote:%d",
		         get_algorithm_state_name(prev_state), get_algorithm_state_name(state),
		         skip_poll, is_quorate, votes, expected_votes, node_count, num_nodes_visible, reserved, reservation_holder, resv_node_visible, cast_vote);
	}

	ENGN_LOG(LOG_TRACE, "SkipPoll:%d CastVote:%d", skip_poll, cast_vote);

	if(!skip_poll) {
		ENGN_LOG(LOG_DEBUG, "Sending cast_vote=%d to votequorum\n", cast_vote);
		vquorum_qdevice_poll(cast_vote);
	} else {
		ENGN_LOG(LOG_DEBUG, "Skipping votequorum poll, not sending cast_vote=%d\n", cast_vote);
	}

	if(prev_state != state) {
		ENGN_LOG(LOG_NOTICE, "State Change: %s->%s", get_algorithm_state_name(prev_state), get_algorithm_state_name(state));
	}

	if(old_node_count != node_count) {
		ENGN_LOG(LOG_NOTICE, "Node Count Change: %d->%d", old_node_count, node_count);
	}

	old_skip_poll = skip_poll;
	old_quorate = is_quorate;
	old_node_count = node_count;
	old_num_nodes_visible = num_nodes_visible;
	old_votes = votes;
	old_expected_votes = expected_votes;
	old_reserved = reserved;
	old_resv_node_visible = resv_node_visible;
	old_cast_vote = cast_vote;

	if(node_keys) {
		free(node_keys);
	}

	good_heartbeat_timeout = gettime_ms() + algorithm_timeout;
}

void algorithm_stop(void)
{
	if(pr_device) {
		persistent_reserve_device_release(pr_device);
		persistent_reserve_device_unregister_key(pr_device);
		persistent_reserve_device_free(pr_device);
	}

	vquorum_qdevice_unregister();
}


static pr_dev_err validate_node_quorum_disk_keys(uint32_t num_node_keys, uint64_t *node_keys)
{
	pr_dev_err err = PR_ERR_OK;
	size_t num_registered_keys = 0;
	uint64_t *registered_keys = NULL;
	err = persistent_reserve_device_get_registered_keys(pr_device, &num_registered_keys, &registered_keys);
	if(PR_ERR_OK != err) {
		ENGN_LOG(LOG_ERR, "Failed to fetch current registered keys.");
		return err;
	}

	for(uint32_t i=0; i < num_node_keys; i++) { // loop over nodes
		uint64_t node_key = node_keys[i];
		bool found = false;
		for(size_t j=0; j < num_registered_keys && !found; j++) {
			found = (node_key == registered_keys[j]);
		}
		if(!found) {
			// TODO: find the node id again
			ENGN_LOG(LOG_ERR, "Node has reservation key %" PERSISTENT_RESERVE_PRI_KEY " which was not registered on the disk\n", node_key);
			err = PR_ERR_FOUND_UNREGISTERED_NODE_KEY;
			goto out;
		}
	}

out:
	if(registered_keys) {
		free(registered_keys);
		registered_keys = NULL;
	}
	return err;
}

static uint64_t gettime_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec*UINT64_C(1000) + t.tv_nsec / UINT64_C(1000000);
}

static const char *get_algorithm_state_name(int s)
{
	static const char *state_strings[] = { "START", "IDLE", "RESERVE_DISK", "RECV_VOTE", "HAVE_QDISK", "RELEASE_QDISK",  "RESIGNED" };
	if(s < 0 || s >= QDISK_NUM_STATES_) {
		ENGN_LOG(LOG_CRIT, "Unknown state number %i!\n", s);
		abort();
	}
	return state_strings[s];
}

uint32_t algorithm_get_heartbeat(void)
{
	return algorithm_heartbeat;
}

const char *algorithm_get_state_name(void)
{
	return get_algorithm_state_name(state);
}

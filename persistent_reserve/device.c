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



#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#include <syslog.h>

#include "persistent_reserve.h"
#include "device.h"
#include "nvme_device.h"
#include "scsi_device.h"

#include "helpers.h"

static uint32_t jenkins_one_at_a_time_hash(const uint8_t *key, size_t length, uint32_t init)
{
	size_t i = 0;
	uint32_t hash = init;
	while(i != length) {
		hash += key[i++];
		hash += hash << 10;
		hash ^= hash >> 6;
	}
	hash += hash << 3;
	hash ^= hash >> 11;
	hash += hash << 15;
	return hash;
}

/**
 * Generated a new key from a key file and then loads it. If the key file doesn't
 *  exist a new one will be created.  If the key file does exist it will be overwrittern.
 */
static pr_dev_err generate_reserve_key(const char *key_path, uint64_t *ikey)
{
	FILE *file = fopen(key_path, "w");
	if(!file) {
		const char *err = strerror(errno);
		pr_log(LOG_ERR, "Failed to write key to file %s (%s)\n", key_path, err);
		return PR_ERR_DEVICE_CANT_READ_KEYFILE;
	}

	char hostname[_POSIX_HOST_NAME_MAX+1] = { 0 };
	gethostname(hostname, _POSIX_HOST_NAME_MAX);
	uint32_t hash1 = jenkins_one_at_a_time_hash((uint8_t*)hostname, strlen(hostname), 0);
	uint32_t hash2 = 0xC0700000 | (0xFFFF & jenkins_one_at_a_time_hash((uint8_t*)hostname, strlen(hostname), 1));
	*ikey = (((uint64_t)hash2) << 32) | hash1;
	fprintf(file, "%" PERSISTENT_RESERVE_PRI_KEY, *ikey);
	fclose(file);

	return PR_ERR_OK;
}

struct persistent_reserve_device *persistent_reserve_device_new(const char *device_path, const char *key_path)
{
	FILE *keyfile = NULL;
	uint64_t ikey = 0;

	if(access(key_path, F_OK) != 0) {
		pr_log(LOG_NOTICE, "Generating key\n");
		generate_reserve_key(key_path, &ikey);
	} else {
		keyfile = fopen(key_path, "r");
		if(!keyfile) {
			const char *errstr = strerror(errno);
			pr_log(LOG_ERR, "Failed to read key from file %s (%s)\n", key_path, errstr);
			return NULL;
		}
		int res = fscanf(keyfile, "%" PERSISTENT_RESERVE_PRI_KEY, &ikey);
		fclose(keyfile);
		if(res == EOF) {
			pr_log(LOG_ERR, "Failed to read key from file %s, bad format in file\n", key_path);
			return NULL;
		}
	}

	return persistent_reserve_device_new_key(device_path, ikey);
}

struct persistent_reserve_device *persistent_reserve_device_new_key(const char *device_path, uint64_t ikey)
{
	struct persistent_reserve_device *handle = NULL;

	if(is_scsi_device_usable(device_path)) {
		pr_log(LOG_NOTICE, "Using SCSI...\n");
		handle = scsi_pr_device_init(device_path, ikey);
	} else if(is_nvme_device_usable(device_path)) {
		pr_log(LOG_NOTICE, "Using NVME...\n");
		handle = nvme_pr_device_init(device_path, ikey);
	} else {
		pr_log(LOG_ERR, "The disk is not a usable SCSI or NVME device. Device detection failed");
		return NULL;
	}

	if(!handle) {
		pr_log(LOG_ERR, "Could not allocate memory for device handle");
	}

	return handle;
}

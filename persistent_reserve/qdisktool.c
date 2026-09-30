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

#include "persistent_reserve.h"

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <syslog.h>

#define PR_KEY_ENV_VAR "DISK_RESERVE_KEY"

static void print_usage(FILE *f, const char *exe)
{
	fprintf(f, "Usage:\n");
	fprintf(f, "\t%s <register|unregister|reserve|release|abort|status> <device> [keyfile]\n", exe);
	fprintf(f, "\t%s <check> <device>\n", exe);
	fprintf(f, "\n");
	fprintf(f, "All commands except check expect a keyfile to exist for the\n");
	fprintf(f, "registration key. They can alternatively read the key from the\n");
	fprintf(f, "environment variable " PR_KEY_ENV_VAR "\n");
	fprintf(f, "If the environment variable is not present and no key file is\n");
	fprintf(f, "given the key will be read from " PERSISTENT_RESERVE_DEFAULT_KEYFILE "\n");
}

int main(int argc, const char *argv[])
{
	struct persistent_reserve_device *handle = NULL;

	if(argc != 3 && argc != 4) {
		fprintf(stderr, "Invalid command syntax\n\n");
		print_usage(stderr, argv[0]);
		return 1;
	}

	bool is_scsi = is_scsi_device(argv[2]);
	bool is_nvme = is_nvme_device(argv[2]);

	if(!(    (is_scsi && is_scsi_device_usable(argv[2])) 
	      || (is_nvme && is_nvme_device_usable(argv[2])) ))
	{
		fprintf(stderr, "The device %s is not a usable SCSI or NVME disk.  Aborting command.\n", argv[2]);
		if(strcmp(argv[1], "check") != 0) {
			fprintf(stderr, "\n");
			print_usage(stderr, argv[0]);
		}
		return 1;
	}

	if(strcmp(argv[1], "check") == 0) {
		pr_dev_err err = PR_ERR_OK;
		uint64_t ikey = 0x7E570000;
		handle = persistent_reserve_device_new_key(argv[2], ikey);

		size_t num_keys = 0;
		uint64_t *keys = NULL;
		err = persistent_reserve_device_get_registered_keys(handle, &num_keys, &keys);
		if(err != PR_ERR_OK) {
			printf("Failed to read keys from device %s\n", argv[2]);
			return 1;
		}

		if(num_keys != 0) {
			printf("Warning: device %s already has %zu keys registered, it may be in use\n", argv[2], num_keys);
		}

		for(size_t i=0; i < num_keys; i++) {
			if(keys[i] == ikey) {
				printf("Error: device %s already has test key %" PERSISTENT_RESERVE_PRI_KEY " registered\n", argv[2], ikey);
				return 1;
			}
		}

		err = persistent_reserve_device_register_key(handle);
		if(err != PR_ERR_OK) {
			printf("Failed to register test key %" PERSISTENT_RESERVE_PRI_KEY " with device %s\n", ikey, argv[2]);
			return 1;
		}

		err = persistent_reserve_device_get_registered_keys(handle, &num_keys, &keys);
		if(err != PR_ERR_OK) {
			printf("Failed to read keys from device %s\n", argv[2]);
			err = persistent_reserve_device_unregister_key(handle);
			if(err != PR_ERR_OK) {
				printf("Failed to un-register test key %" PERSISTENT_RESERVE_PRI_KEY " from device %s\n", ikey, argv[2]);
			}
			return 1;
		}

		bool found_test_key = false;
		for(size_t i=0; i < num_keys; i++) {
			found_test_key = found_test_key || (keys[i] == ikey);
		}
		if(!found_test_key) {
			printf("Test key %" PERSISTENT_RESERVE_PRI_KEY " not listed by device %s after registration\n", ikey, argv[2]);
		}

		err = persistent_reserve_device_register_key(handle);
		if(err != PR_ERR_OK) {
			printf("Failed to un-register test key %" PERSISTENT_RESERVE_PRI_KEY " from device %s\n", ikey, argv[2]);
			return 1;
		}

		printf("Device %s is usable\n", argv[2]);
		return 0;
	}

	if(argc == 4) {
		handle = persistent_reserve_device_new(argv[2], argv[3]);
	} else if(getenv(PR_KEY_ENV_VAR)) {
		const char *key_str = getenv(PR_KEY_ENV_VAR);
		uint64_t ikey = 0;
		if(EOF == sscanf(key_str, "%" PERSISTENT_RESERVE_PRI_KEY, &ikey)) {
			fprintf(stderr, "Bad key string '%s' in environment variable PR_KEY_ENV_VAR\n", key_str);
			return 1;
		}
		handle = persistent_reserve_device_new_key(argv[2], ikey);
	} else {
		handle = persistent_reserve_device_new(argv[2], PERSISTENT_RESERVE_DEFAULT_KEYFILE);
	}

	if(!handle) {
		fprintf(stderr, "Failed to open device %s. Aborting command.\n\n", argv[2]);
		return 1;
	}

    pr_dev_err err = PR_ERR_OK;

	if(strcmp(argv[1], "register") == 0) {
		err = persistent_reserve_device_register_key(handle);
	} else if(strcmp(argv[1], "unregister") == 0) {
		err = persistent_reserve_device_unregister_key(handle);
	} else if(strcmp(argv[1], "reserve") == 0) {
		err = persistent_reserve_device_reserve(handle);
	} else if(strcmp(argv[1], "release") == 0) {
		err = persistent_reserve_device_release(handle);
	} else if(strcmp(argv[1], "abort") == 0) {
		err = persistent_reserve_device_abort(handle);
	} else if(strcmp(argv[1], "status") == 0) {
		uint64_t res_holder_key = 0;
		int is_reserved = persistent_reserve_device_is_reserved(handle);
		if(is_reserved > 0) {
			if(persistent_reserve_device_reservation_owner_key(handle, &res_holder_key)) {
				fprintf(stderr, "Device is reserved, but there was an error fetching the holder.\n");
			}
			else {
				printf("Device is reserved, holder will be marked with a *\n");
			}
		} else {
			printf("Device is not reserved.\n\n");
		}

		size_t num_keys = 0;
		uint64_t *keys = NULL;
		persistent_reserve_device_get_registered_keys(handle, &num_keys, &keys);
		printf("Registered keys:\n");
		for(size_t i=0; i < num_keys; i++) {
			printf("    %" PRIx64 "%s\n", keys[i], ((keys[i] == res_holder_key)? " *": ""));
		}
		printf("\n");
	} else {
		fprintf(stderr, "Invalid action: '%s'\n\n", argv[1]);
		print_usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

    if(err != PR_ERR_OK) {
        fprintf(stderr, "%s\n", pr_strerr(err));
        return EXIT_FAILURE;
    }
	return EXIT_SUCCESS;
}

// wwn:6001405ce5b94d3feb649d2ab4952d02
// nvme:WD_PC_SN740_SDDQNQD-256G-1001_22320H808327

// Provide a log function for the code in persistant_reserve to use, just output warnings and errors to stderr
void pr_log(int level, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	if(level <= LOG_WARNING) vfprintf(stderr, fmt, args);
	va_end(args);
}

/*
 * (C) Copyright 2015
 * Stephen Shelton, NETSCOUT, Inc.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/*
 * Memory Functions
 *
 * Copied and modified from cmd_mem.c, which is (C) Wolfgang Denk
 */

#include <common.h>
#include <bootretry.h>
#include <cli.h>
#include <command.h>
#include <hash.h>
#include <watchdog.h>
#include <asm/io.h>
#include <linux/compiler.h>

DECLARE_GLOBAL_DATA_PTR;

static ulong mfg_mem_test(vu_long *buf, ulong start_addr, ulong end_addr,
			  vu_long *dummy);

/* test
 */
static int do_mfg_test(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
    printf("Performing manufacturing test...\n");

    // configure mem test
    vu_long *buf, *dummy;
    ulong num_errors;
    ulong start = 0x10000000;
    ulong end = 0x4f000000;

	buf = map_sysmem(start, end - start);

	dummy = map_sysmem(CONFIG_SYS_MEMTEST_SCRATCH, sizeof(vu_long)); // copying from cmd_mem.c... not sure why this is necessary
    num_errors = mfg_mem_test(buf, start, end, dummy);

    printf("mem test complete. num errors: %lu\n", num_errors);

    bool passed = (num_errors == 0);

    // write results to eeprom
    int result = 0;
    result = env_init();
    if (result)
    {
        printf("Error: unable to initialize environment!\n");
        return -1;
    }

    // TODO: write results to eeprom
    result = setenv("mfg_test_result", (passed ? "passed" : "failed"));
    if (result)
    {
        printf("Error: unable to set env var!\n");
        return -1;
    }

    result = saveenv();
    if (result)
    {
        printf("Error: unable to save environment to eeprom!\n");
        return -1;
    }

    return 0;
}

static ulong mfg_mem_test(vu_long *buf, ulong start_addr, ulong end_addr,
			  vu_long *dummy)
{
	printf("mfg_mem_test\n");
	vu_long *addr;
	ulong errs = 0;
	ulong val, readback;
	int j;
	vu_long offset;
	vu_long test_offset;
	vu_long pattern;
	vu_long temp;
	vu_long anti_pattern;
	vu_long num_words;
	static const ulong bitpattern[] = {
		0x00000001,	/* single bit */
		0x00000003,	/* two adjacent bits */
		0x00000007,	/* three adjacent bits */
		0x0000000F,	/* four adjacent bits */
		0x00000005,	/* two non-adjacent bits */
		0x00000015,	/* three non-adjacent bits */
		0x00000055,	/* four non-adjacent bits */
		0xaaaaaaaa,	/* alternating 1/0 */
	};

	num_words = (end_addr - start_addr) / sizeof(vu_long);

	/*
	 * Data line test: write a pattern to the first
	 * location, write the 1's complement to a 'parking'
	 * address (changes the state of the data bus so a
	 * floating bus doesn't give a false OK), and then
	 * read the value back. Note that we read it back
	 * into a variable because the next time we read it,
	 * it might be right (been there, tough to explain to
	 * the quality guys why it prints a failure when the
	 * "is" and "should be" are obviously the same in the
	 * error message).
	 *
	 * Rather than exhaustively testing, we test some
	 * patterns by shifting '1' bits through a field of
	 * '0's and '0' bits through a field of '1's (i.e.
	 * pattern and ~pattern).
	 */
	addr = buf;
	for (j = 0; j < sizeof(bitpattern) / sizeof(bitpattern[0]); j++) {
		val = bitpattern[j];
		for (; val != 0; val <<= 1) {
			*addr = val;
			*dummy  = ~val; /* clear the test data off the bus */
			readback = *addr;
			if (readback != val) {
				printf("FAILURE (data line): "
					"expected %08lx, actual %08lx\n",
						val, readback);
				errs++;
				if (ctrlc())
					return -1;
			}
			*addr  = ~val;
			*dummy  = val;
			readback = *addr;
			if (readback != ~val) {
				printf("FAILURE (data line): "
					"Is %08lx, should be %08lx\n",
						readback, ~val);
				errs++;
				if (ctrlc())
					return -1;
			}
		}
	}

	/*
	 * Based on code whose Original Author and Copyright
	 * information follows: Copyright (c) 1998 by Michael
	 * Barr. This software is placed into the public
	 * domain and may be used for any purpose. However,
	 * this notice must not be changed or removed and no
	 * warranty is either expressed or implied by its
	 * publication or distribution.
	 */

	/*
	* Address line test

	 * Description: Test the address bus wiring in a
	 *              memory region by performing a walking
	 *              1's test on the relevant bits of the
	 *              address and checking for aliasing.
	 *              This test will find single-bit
	 *              address failures such as stuck-high,
	 *              stuck-low, and shorted pins. The base
	 *              address and size of the region are
	 *              selected by the caller.

	 * Notes:	For best results, the selected base
	 *              address should have enough LSB 0's to
	 *              guarantee single address bit changes.
	 *              For example, to test a 64-Kbyte
	 *              region, select a base address on a
	 *              64-Kbyte boundary. Also, select the
	 *              region size as a power-of-two if at
	 *              all possible.
	 *
	 * Returns:     0 if the test succeeds, 1 if the test fails.
	 */
	pattern = (vu_long) 0xaaaaaaaa;
	anti_pattern = (vu_long) 0x55555555;

	debug("%s:%d: length = 0x%.8lx\n", __func__, __LINE__, num_words);
	/*
	 * Write the default pattern at each of the
	 * power-of-two offsets.
	 */
	for (offset = 1; offset < num_words; offset <<= 1)
		addr[offset] = pattern;

	/*
	 * Check for address bits stuck high.
	 */
	test_offset = 0;
	addr[test_offset] = anti_pattern;

	for (offset = 1; offset < num_words; offset <<= 1) {
		temp = addr[offset];
		if (temp != pattern) {
			printf("\nFAILURE: Address bit stuck high @ 0x%.8lx:"
				" expected 0x%.8lx, actual 0x%.8lx\n",
				start_addr + offset*sizeof(vu_long),
				pattern, temp);
			errs++;
			if (ctrlc())
				return -1;
		}
	}
	addr[test_offset] = pattern;
	WATCHDOG_RESET();

	/*
	 * Check for addr bits stuck low or shorted.
	 */
	for (test_offset = 1; test_offset < num_words; test_offset <<= 1) {
		addr[test_offset] = anti_pattern;

		for (offset = 1; offset < num_words; offset <<= 1) {
			temp = addr[offset];
			if ((temp != pattern) && (offset != test_offset)) {
				printf("\nFAILURE: Address bit stuck low or"
					" shorted @ 0x%.8lx: expected 0x%.8lx,"
					" actual 0x%.8lx\n",
					start_addr + offset*sizeof(vu_long),
					pattern, temp);
				errs++;
				if (ctrlc())
					return -1;
			}
		}
		addr[test_offset] = pattern;
	}


	return 0;
}

U_BOOT_CMD(
	mfg_test,	3,	1,	do_mfg_test,
	"perform manufacturing test",
    "perform manufacturing test"
);

//
//  m1_test.h
//  DoritOS
//
//  Created by Sebastian Winberg on 06.10.17.
//

#ifndef m1_test_h
#define m1_test_h

#include <stdio.h>
#include <stdlib.h>

#include <aos/aos.h>
#include <aos/waitset.h>
#include <aos/morecore.h>
#include <aos/paging.h>

#include <mm/mm.h>
#include "mem_alloc.h"

errval_t test_alloc_free(size_t);

errval_t test_alloc_free_alloc_free(size_t, size_t);

errval_t test_alloc_free_free(size_t);

errval_t test_alloc_n_free_n(int, size_t);

errval_t test_coalescing(size_t, size_t, size_t);

errval_t test_ram_leak(int, size_t);

errval_t test_inc_n_by_k(int, int);

errval_t test_random_seq(void);

errval_t test_slot_alloc_n(int);

errval_t test_frame_alloc(size_t);

errval_t test_frame_alloc_n(int, size_t);

void run_all_m1_tests(void);

#define PRINT_TEST_NAME         printf("Test %s: ", __FUNCTION__)
#define RETURN_TEST_SUCCESS        do { printf("\033[37m\033[42mSUCCESS\033[49m\033[39m\n"); return SYS_ERR_OK; } while(0)

#endif /* test_h */

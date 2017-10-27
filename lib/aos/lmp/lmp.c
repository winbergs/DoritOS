#include <aos/aos.h>
#include <aos/waitset.h>
#include <aos/lmp.h>

#define MAX_ALLOCATION 100000000

#define PRINT_DEBUG 1

/* ========== Server ========== */
void lmp_server_dispatcher(void *arg) {

#if PRINT_DEBUG
    debug_printf("LMP Message Received!\n");
#endif

    errval_t err;

    struct lmp_chan *lc = (struct lmp_chan *) arg;
    struct capref cap;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;

    err = lmp_chan_recv(lc, &msg, &cap);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));

        err = lmp_chan_register_recv(lc, get_default_waitset(), MKCLOSURE(lmp_server_dispatcher, (void *) lc));
        if (err_is_fail(err)) {
            debug_printf("%s\n", err_getstring(err));
        }

        return;
    }

    // Check message type and handle
    switch (msg.words[0]) {
        case LMP_RequestType_Register:
#if PRINT_DEBUG
            debug_printf("Registration Message!\n");
#endif
            lmp_server_register(lc, cap);
            break;
        case LMP_RequestType_MemoryAlloc:
#if PRINT_DEBUG
            debug_printf("Memory Alloc Message!\n");
#endif
            break;
        case LMP_RequestType_MemoryFree:
#if PRINT_DEBUG
            debug_printf("Memory Free Message!\n");
#endif
            break;
        case LMP_RequestType_Spawn:
#if PRINT_DEBUG
            debug_printf("Spawn Message!\n");
#endif
            break;
        case LMP_RequestType_Terminal:
#if PRINT_DEBUG
            debug_printf("Terminal Message!\n");
#endif
            break;
        default:
#if PRINT_DEBUG
            debug_printf("Invalid Message!\n");
#endif
    }


    err = lmp_chan_register_recv(lc, get_default_waitset(), MKCLOSURE(lmp_server_dispatcher, (void *) lc));
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
    }
}

void lmp_server_register(struct lmp_chan *lc, struct capref cap) {
    errval_t err;

    lc->remote_cap = cap;

    err = lmp_chan_alloc_recv_slot(lc);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
    }

    err = lmp_chan_send1(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, SYS_ERR_OK);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
    }
}

errval_t lmp_server_memory_alloc(struct lmp_chan *lc, size_t bytes, size_t align) {
    
    errval_t err = SYS_ERR_OK;
    
    // Checking for invalid allocation size or alignment
    if (bytes == 0 || align == 0) {
        debug_printf("size or alignment is zero\n");
        err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, LMP_RequestType_MemoryAlloc, SYS_ERR_INVALID_SIZE);
        return err;
    }
    
    // TODO: Implement allocation policy for processes
    
    // Checking if requested allocation size is too big
    if (bytes > MAX_ALLOCATION) {
        debug_printf("requested size too big\n");
        err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, LMP_RequestType_MemoryAlloc, SYS_ERR_INVALID_SIZE);
        return err;
    }
    
    // Allocating ram capability with size bytes and alignment align
    struct capref ram;
    err = ram_alloc_aligned(&ram, bytes, align);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
    }
    
    // Responding by sending the ram capability back
    err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, ram, LMP_RequestType_MemoryAlloc, SYS_ERR_OK);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
    }
    
    // Deleting the ram capability
    cap_delete(ram);
    
    // Freeing the slot
    slot_free(ram);
    
    return err;
    
}

static ram_free_handler_t ram_free_handler;

// Registering ram_free_handler function
void register_ram_free_handler(ram_free_handler_t ram_free_function) {
    ram_free_handler = ram_free_function;
}

errval_t lmp_server_memory_free(struct lmp_chan *lc, struct capref cap, size_t bytes) {
    
    errval_t err = SYS_ERR_OK;
    
    // Freeing ram capability
    err = ram_free_handler(cap, bytes);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
        err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, LMP_RequestType_MemoryFree, MM_ERR_MM_FREE);
        return err;
    }

    // Responding that freeing ram capability was successful
    err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, LMP_RequestType_MemoryFree, SYS_ERR_OK);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
    }

    return err;
    
}


void lmp_server_spawn(struct lmp_chan *lc, struct capref cap) {

}

void lmp_server_terminal(struct lmp_chan *lc, struct capref cap) {

}

/* ========== Client ========== */

void lmp_client_recv(struct lmp_chan *arg, struct capref *cap, struct lmp_recv_msg *msg) {
    int done = 0;
    errval_t err;

    struct lmp_chan *lc = (struct lmp_chan *) arg;

    err = lmp_chan_register_recv(lc, get_default_waitset(), MKCLOSURE(lmp_client_wait, &done));
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
        return;
    }

    while (!done) {
        event_dispatch(get_default_waitset());
    }

    lmp_chan_recv(lc, msg, cap);
}

void lmp_client_wait(void *arg) {
    *(int *)arg = 1;
}

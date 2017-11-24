#ifndef _AOS_LMP_H_
#define _AOS_LMP_H_

#include <aos/aos.h>
#include <aos/deferred.h>

/*
 * LMP Request Protocol
 *
 * ==== Number ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_Number
 * arg1: uintptr_t Number
 *
 * cap: NULL_CAP
 *
 * ==== ShortBuf ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_ShortBuf
 * arg1: size_t length of the buffer
 * arg2-8: void * Buffer
 *
 * cap: NULL_CAP
 *
 * ==== FrameSend ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_FrameSend
 * arg1: size_t Size of frame
 *
 * cap: Frame containing data
 *
 * ==== Register ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_Register
 *
 * cap: capability to endpoint of client
 *
 * ==== Memory Alloc ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_MemoryAlloc
 * arg1: bytes
 * arg2: align
 *
 * cap: NULL_CAP
 *
 * ==== Memory Free ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_MemoryFree
 * arg1: bytes
 *
 * cap: capability to memory to free
 *
 * ==== Spawn ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_Spawn
 * arg1: coreid_t Core ID
 * arg2-8: char[] Name
 *
 * cap: NULL_CAP
 *
 * ==== NameLookup ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_NameLookup
 * arg1: domainid_t PID of process to be looked up
 *
 * cap: NULL_CAP
 *
 * ==== PidDiscover ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_PidDiscover
 *
 * cap: NULL_CAP
 *
 * ==== Terminal Get Char ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_TerminalGetChar
 *
 * cap: NULL_CAP
 *
 * ==== Terminal Put Char ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_TerminalPutChar
 * arg1: char Char
 *
 * cap: NULL_CAP
 *
 */

/*
 * LMP Response Protocol
 *
 * ==== Number ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_Number
 * arg1: uintptr_t Number
 *
 * cap: NULL_CAP
 *
 * ==== ShortBuf ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_ShortBuf
 * arg1: errval_t Status code
 * arg2: size_t Received content length
 *
 * cap: NULL_CAP
 *
 * ==== FrameSend ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_FrameSend
 * arg1: errval_t Status code
 * arg2: size_t Received content length
 *
 * cap: NULL_CAP
 *
 * ==== Register ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_Register
 * arg1: errval_t Status code
 *
 * cap: NULL_CAP
 *
 * ==== Memory Alloc ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_MemoryAlloc
 * arg1: errval_t Status code
 *
 * cap: RAM capability to allocated memory
 *
 * ==== Memory Free ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_MemoryFree
 * arg1: errval_t Status code
 *
 * cap: NULL_CAP
 *
 * ==== Spawn ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_Spawn
 * arg1: errval_t Status code
 * arg2: domainid_t Process ID of new process
 *
 * cap:
 *
 * ==== PidDiscover ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_PidDiscover
 * arg1: size_t Number of PIDs
 *
 * cap: NULL_CAP
 *
 * ==== Terminal Get Char ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_TerminalGetChar
 * arg1: errval_t Error
 * arg2: char Char
 *
 * cap: NULL_CAP
 *
 * ==== Terminal Put Char ====
 *
 * arg0: enum lmp_request_type RequestType = LMP_RequestType_TerminalPutChar
 * arg1: errval_t Error
 *
 * cap: NULL_CAP
 *
 */

extern unsigned serial_console_port;

enum lmp_request_type {
    LMP_RequestType_NULL = 0,
    LMP_RequestType_Number,
    LMP_RequestType_ShortBuf,
    LMP_RequestType_FrameSend,
    LMP_RequestType_Register,
    LMP_RequestType_MemoryAlloc,
    LMP_RequestType_MemoryFree,
    LMP_RequestType_Spawn,
    LMP_RequestType_NameLookup,
    LMP_RequestType_PidDiscover,
    LMP_RequestType_TerminalGetChar,
    LMP_RequestType_TerminalPutChar,
    LMP_RequestType_Echo,
    LMP_RequestType_UmpBind,
    LMP_RequestType_GetDeviceCap
};

typedef errval_t (*lmp_server_spawn_handler)(char *name, coreid_t coreid, domainid_t *pid);

typedef errval_t (*ram_free_handler_t)(struct capref, size_t size);


// Server side
void lmp_server_dispatcher(void *arg);
void lmp_server_register(struct lmp_chan *lc, struct capref cap);
errval_t lmp_server_memory_alloc(struct lmp_chan *lc, size_t bytes, size_t align);
void register_ram_free_handler(ram_free_handler_t ram_free_function);
errval_t lmp_server_memory_free(struct lmp_chan *lc, struct capref cap, size_t bytes);
void lmp_server_spawn(struct lmp_chan *lc, uintptr_t *args);
void lmp_server_spawn_register_handler(lmp_server_spawn_handler handler);
errval_t lmp_server_pid_discovery(struct lmp_chan *lc);
void lmp_server_terminal_putchar(struct lmp_chan *lc, char c);
void lmp_server_terminal_getchar(struct lmp_chan *lc);

// Client side
void lmp_client_recv(struct lmp_chan *arg, struct capref *cap, struct lmp_recv_msg *msg);
void lmp_client_wait(void *arg);

// Send a string on a specific channel (automatically select protocol)
errval_t lmp_send_string(struct lmp_chan *lc, const char *string);

// Send a short buffer (using LMP arguments)
errval_t lmp_send_short_buf(struct lmp_chan *lc, void *buf, size_t size);

// Send an entire frame capability
errval_t lmp_send_frame(struct lmp_chan *lc, struct capref frame_cap, size_t frame_size);

// Blocking call to receive a string on a channel (automatically select protocol)
errval_t lmp_recv_string(struct lmp_chan *lc, char **string);

// Process a string received through a message (automatically select protocol)
errval_t lmp_recv_string_from_msg(struct lmp_chan *lc, struct capref cap,
                                  uintptr_t *words, char **string);

// Receive a short buffer on a channel (using LMP arguments)
errval_t lmp_recv_short_buf(struct lmp_chan *lc, void **buf, size_t *size);

// Process a short buffer received through a message (using LMP arguments)
errval_t lmp_recv_short_buf_from_msg(struct lmp_chan *lc, uintptr_t *words,
                                     void **buf, size_t *size);

// Receive a frame on a channel
errval_t lmp_recv_frame(struct lmp_chan *lc, struct capref *frame_cap, size_t *size);

// Process a frame received through a message
errval_t lmp_recv_frame_from_msg(struct lmp_chan *lc, struct capref msg_cap,
                                 uintptr_t *words, struct capref *frame_cap,
                                 size_t *size);



#endif

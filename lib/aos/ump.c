//
//  ump.c
//  DoritOS
//

#include <string.h>

#include <aos/capabilities.h>
#include <machine/atomic.h>

#include "aos/ump.h"

// Struct for UMP channel with the other init (only in init)
struct ump_chan init_uc;

// Initialize a UMP channel
void ump_chan_init(struct ump_chan *chan, uint8_t buf_select) {
    
    // Set buffer selector
    assert(buf_select < 2);
    chan->buf_select = buf_select;
    
    // Set the counters to zero
    chan->tx_counter = 0;
    chan->rx_counter = 0;
    chan->ack_counter = 0;
    
}

// Send a buffer of at most UMP_SLOT_DATA_BYTES bytes on the URPC channel
errval_t ump_send_one(struct ump_chan *chan, const void *buf, size_t size,
                       ump_msg_type_t msg_type, uint8_t last) {
    
    // Check for invalid sizes
    //assert(size <= UMP_SLOT_DATA_BYTES);
    if (size > UMP_SLOT_DATA_BYTES) {
        return LIB_ERR_UMP_BUFSIZE_INVALID;
    }
    
    // Get the correct UMP buffer
    struct ump_buf *tx_buf = chan->buf + chan->buf_select;
    
    // Make sure there is space in the ring buffer and wait otherwise
    while (tx_buf->slots[chan->tx_counter].valid) ;
    
    // Memory barrier
    dmb();
    
    // Copy data to the slot
    memcpy(tx_buf->slots[chan->tx_counter].data, buf, size);
    tx_buf->slots[chan->tx_counter].msg_type = msg_type;
    tx_buf->slots[chan->tx_counter].last = last;
    
    // Memory barrier
    dmb();
    
    // Mark the message as valid
    tx_buf->slots[chan->tx_counter].valid = 1;
    
    // Set the index of the next slot to use for sending
    chan->tx_counter = (chan->tx_counter + 1) % UMP_NUM_SLOTS;
    
    return SYS_ERR_OK;
    
}

// Send a buffer on the UMP channel
errval_t ump_send(struct ump_chan *chan, const void *buf, size_t size,
                   ump_msg_type_t msg_type) {

    errval_t err = SYS_ERR_OK;

    while (size > 0) {
        
        size_t msg_size = MIN(size, UMP_SLOT_DATA_BYTES);
        
        // Send fragment of entire message
        err = ump_send_one(chan,
                            buf,
                            msg_size,
                            msg_type,
                            size <= UMP_SLOT_DATA_BYTES);
        if (err_is_fail(err)) {
            return err;
        }
        
        buf += msg_size;
        size -= msg_size;
        
    }
    
    return err;
    
}

// Receive a buffer of UMP_SLOT_DATA_BYTES bytes on the UMP channel
errval_t ump_recv_one(struct ump_chan *chan, void *buf,
                       ump_msg_type_t* msg_type, uint8_t *last) {
    
    // Get the correct UMP buffer
    struct ump_buf *rx_buf = chan->buf + !chan->buf_select;
    
    // Check if there is a new message
    if (!rx_buf->slots[chan->rx_counter].valid) {
        return LIB_ERR_NO_UMP_MSG;
    }

    // Memory barrier
    dmb();
    
    // Copy data from the slot
    memcpy(buf, rx_buf->slots[chan->rx_counter].data, UMP_SLOT_DATA_BYTES);
    *msg_type = rx_buf->slots[chan->rx_counter].msg_type;
    *last = rx_buf->slots[chan->rx_counter].last;

    // Memory barrier
    dmb();

    // Mark the message as invalid
    rx_buf->slots[chan->rx_counter].valid = 0;

    // Set the index of the next slot to read
    chan->rx_counter = (chan->rx_counter + 1) % UMP_NUM_SLOTS;

    return SYS_ERR_OK;
    
}

// Receive a buffer of `size` bytes on the UMP channel
errval_t ump_recv(struct ump_chan *chan, void **buf, size_t *size,
                  ump_msg_type_t* msg_type) {
    errval_t err = SYS_ERR_OK;

    uint8_t last;

    // Allocate memory for the first message
    *size = UMP_SLOT_DATA_BYTES;
    *buf = malloc(*size);
    if (*buf == NULL) {
        return LIB_ERR_MALLOC_FAIL;
    }

    // Check that we have received an initial message
    err = ump_recv_one(chan, *buf, msg_type, &last);
    if (err_is_fail(err)) {
        free(*buf);
        return err;
    }

    // Loop until we received the final message
    while (!last) {

        ump_msg_type_t this_msg_type;

        // Allocate more space for the next message
        *size += UMP_SLOT_DATA_BYTES;
        *buf = realloc(*buf, *size);
        if (*buf == NULL) {
            return LIB_ERR_MALLOC_FAIL;
        }

        // Receive the next message
        err = ump_recv_one(chan,
                            *buf + *size - UMP_SLOT_DATA_BYTES,
                            &this_msg_type,
                            &last);
        if (err == LIB_ERR_NO_UMP_MSG) {
            continue;
        }
        else if (err_is_fail(err)) {
            free(*buf);
            return err;
        }
        
        // Check the message types are consistent
        assert(this_msg_type == *msg_type);
        
    }
    
    return err;
    
}

void ump_recv_blocking(struct ump_chan *chan, void **buf, size_t *size,
                       ump_msg_type_t *msg_type) {
    errval_t err;
    do {
        err = ump_recv(chan, buf, size, msg_type);
    } while(err == LIB_ERR_NO_UMP_MSG);
}

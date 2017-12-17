//
//  slip.c
//  DoritOS
//
//  Created by Carl Friess on 16/12/2017.
//  Copyright © 2017 Carl Friess. All rights reserved.
//

#include "slip.h"
#include "ip.h"

#include <stdlib.h>
#include <string.h>
#include <bitmacros.h>
#include <assert.h>

#include <aos/aos.h>


static void slip_parse_raw_ip_packet(struct ip_packet_raw *raw_packet);


static struct ip_packet_raw current_packet = {
    .buf = NULL,
    .len = 0
};

static enum {
    PARSER_STATE_NORMAL,
    PARSER_STATE_ESC
} parser_state = PARSER_STATE_NORMAL;

int slip_init(void) {
    
    // Allocating space for receive buffer (max ip packet size)
    current_packet.buf = calloc(MAX_IP_PACKET_SIZE, 1);
    current_packet.len = 0;
    if (!current_packet.buf) {
        return -1;
    }
    
    return 0;
    
}

// Receive and parse bytes from the network
void slip_recv(uint8_t *buf, size_t len) {
    
    // Iterate through all received bytes
    while (len--) {
        //debug_printf("%x, %zu\n", *buf, current_packet.len);
        switch (*buf) {
            
            // Handle end of packet
            case SLIP_END:
                // Sanity check
                assert(parser_state == PARSER_STATE_NORMAL);
                
                // Parse the raw IP packet
                slip_parse_raw_ip_packet(&current_packet);
                
                // Reset the input parser
                parser_state = PARSER_STATE_NORMAL;
                current_packet.buf -= current_packet.len;
                current_packet.len = 0;
                continue;
            
            // Handle escape sequences
            case SLIP_ESC:
                assert(parser_state == PARSER_STATE_NORMAL);
                parser_state = PARSER_STATE_ESC;
                buf++;
                continue;
                
            case SLIP_ESC_END:
                if (parser_state == PARSER_STATE_ESC) {
                    *buf = SLIP_END;
                    parser_state = PARSER_STATE_NORMAL;
                }
                break;
                
            case SLIP_ESC_ESC:
                if (parser_state == PARSER_STATE_ESC) {
                    *buf = SLIP_ESC;
                    parser_state = PARSER_STATE_NORMAL;
                }
                break;
                
            case SLIP_ESC_NUL:
                if (parser_state == PARSER_STATE_ESC) {
                    *buf = 0x00;
                    parser_state = PARSER_STATE_NORMAL;
                }
                break;
            
            default:
                // Make sure escape sequences are correct
                assert(parser_state == PARSER_STATE_NORMAL);
        }
        
        // Copy the received byte
        *(current_packet.buf++) = *(buf++);
        current_packet.len++;

    }
    
}


static void slip_parse_raw_ip_packet(struct ip_packet_raw *raw_packet) {
    
    debug_printf("RECEIVED PACKET (length: %zu)\n", raw_packet->len);
    
    ip_handle_packet(raw_packet->buf - raw_packet->len, raw_packet->len);
    
};
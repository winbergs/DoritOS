//
//  icmp.c
//  DoritOS
//
//  Created by Carl Friess on 17/12/2017.
//  Copyright © 2017 Carl Friess. All rights reserved.
//

#include "icmp.h"
#include "ip.h"

#include <aos/aos.h>

#include <string.h>
#include <assert.h>

#include <netutil/htons.h>
#include <netutil/checksum.h>

// Parse and validate an ICMP header
int icmp_parse_header(uint8_t *buf, size_t len, struct icmp_header *header) {
    
    // Parse all fields
    header->type = buf[0];
    header->code = buf[1];
    header->checksum = lwip_ntohs(*(uint16_t *)(buf + 2));
    header->data = lwip_ntohl(*(uint32_t *)(buf + 4));
    
    // Compute and verify checksum
    uint16_t checksum = inet_checksum((void *) buf, len);
    if (checksum != 0xFFFF && checksum != 0x0) {
        return 1;
    }
    
    return 0;
    
}

// Encode an ICMP header
void icmp_encode_header(struct icmp_header *header, uint8_t *buf) {
    
    // Set data fields
    buf[0] = header->type;
    buf[1] = header->code;
    *(uint16_t *)(buf + 2) = ~header->checksum;
    *(uint32_t *)(buf + 4) = lwip_htonl(header->data);
    
    // Compute and set checksum
    header->checksum = inet_checksum((void *) buf, 8);
    *(uint16_t *)(buf + 2) = header->checksum;
    
}

// Handle an ICMP echo request
static void icmp_handle_echo_req(uint32_t src_ip,
                                 struct icmp_header *header,
                                 uint8_t *buf,
                                 size_t len) {

    struct icmp_header reply_header;
    
    // Build header
    reply_header.type = ICMP_MSG_TYPE_ECHO_REPLY;
    reply_header.code = 0;
    reply_header.data = header->data;
    
    // Compute checksum for payload
    reply_header.checksum = inet_checksum((void *) buf, len);
    
    // Send IP header
    ip_send_header(src_ip, IP_PROTOCOL_ICMP, 8 + len);
    
    // Encode header and send it
    uint8_t *reply_buf = malloc(8);
    assert(reply_buf);
    icmp_encode_header(&reply_header, reply_buf);
    ip_send(reply_buf, 8, false);
    free(reply_buf);
    
    // Send payload
    ip_send(buf, len, true);
    
}

void icmp_handle_packet(uint32_t src_ip, uint8_t *buf, size_t len) {
    
    // Discard too small messages
    if (len < 8) {
        debug_printf("INVALID ICMP MESSAGE: TOO SHORT\n");
        return;
    }
    
    // Parse and check the ICMP header
    struct icmp_header header;
    int e;
    if ((e = icmp_parse_header(buf, len, &header))) {
        debug_printf("INVALID ICMP MESSAGE: %d\n", e);
        return;
    }
        
    switch (header.type) {
        case ICMP_MSG_TYPE_ECHO_REQ:
            if (header.code != 0) {
                debug_printf("INVALID ICMP MESSAGE: WRONG CODE\n");
                return;
            }
            icmp_handle_echo_req(src_ip,
                                 &header,
                                 buf + 8,
                                 len - 8);
            break;
            
        default:
            debug_printf("Unknown ICMP message type (%d)!\n",
                         (int) header.type);
            break;
    }
    
}

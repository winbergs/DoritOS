#include <stdlib.h>
#include <aos/aos.h>
#include <aos/lmp.h>
#include <spawn/spawn.h>
#include <aos/urpc.h>
#include <aos/urpc_protocol.h>

#include "spawn_serv.h"

static struct ump_chan *ump_chan;

static errval_t request_remote_spawn(char *name, coreid_t coreid,
                                     domainid_t terminal_pid, domainid_t *pid) {
    
    errval_t err = SYS_ERR_OK;
    
    // Build message
    size_t msg_size = sizeof(domainid_t) + strlen(name) + 1;
    char *msg = malloc(msg_size);
    if (!msg) {
        return LIB_ERR_MALLOC_FAIL;
    }
    *(domainid_t *) msg = terminal_pid;
    strcpy((char *) msg + sizeof(domainid_t), name);
    
    // Send request to spawn server on other core
    err = ump_send(ump_chan, msg, msg_size, UMP_MessageType_Spawn);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
    }
    
    // Free the message
    free(msg);
    
    // Waiting for response
    size_t retsize;
    ump_msg_type_t msg_type;
    
    struct urpc_spaw_response *recv_buf;

    // Repeat until a message is received
    do {
        
        // Receive response of spawn server and save it in recv_buf
        err = ump_recv(ump_chan, (void **) &recv_buf, &retsize, &msg_type);
        
        // Check for a process register message
        if (err_is_ok(err) && msg_type == UMP_MessageType_RegisterProcess) {
            
            // Pass message to URPC handler
            urpc_register_process_handler(ump_chan,
                                          (void *)recv_buf,
                                          retsize,
                                          msg_type);
            free(recv_buf);
            
            // Continue looping
            err = LIB_ERR_NO_UMP_MSG;
        }
        
    } while (err == LIB_ERR_NO_UMP_MSG);
    
    // Check that receive was successful
    if (err_is_fail(err)) {
        return err;
    }
    
    // TODO: Handle incorrect UMP_MessageType gracefully
    assert(msg_type == UMP_MessageType_SpawnAck);
    
    // Set pid of spawned process
    *pid = recv_buf->pid;
    
    // Returned error code
    err = recv_buf->err;
    
    // Free memory of recv_buf
    free(recv_buf);
    
    // Return status
    return err;
    
}

errval_t spawn_serv_handler(char *name, coreid_t coreid,
                            domainid_t terminal_pid, domainid_t *pid) {
    errval_t err;
    
    // Check if spawn request for this core
    if (coreid != disp_get_core_id()) {
        
        return request_remote_spawn(name, coreid, terminal_pid, pid);
    
    }
    
    // Allocate spawninfo
    struct spawninfo *si = (struct spawninfo *) malloc(sizeof(struct spawninfo));
    if (si == NULL) {
        // Free spawn info
        free(si);
        return LIB_ERR_MALLOC_FAIL;
    }
    
    // Spawn memeater
    err = spawn_load_by_name(name, si, terminal_pid);
    if (err_is_fail(err)) {
        debug_printf("%s\n", err_getstring(err));
        // Free spawn info
        free(si);
        return err;
    }
    
    // Return the new process id
    *pid = si->pi->pid;
    
    // Free the process info for memeater
    free(si);
    
    //print_process_list();
 
    return err;
    
}

errval_t spawn_serv_init(struct ump_chan *chan) {
    
    ump_chan = chan;
    
    lmp_server_spawn_register_handler(spawn_serv_handler);
    
    return SYS_ERR_OK;
    
}

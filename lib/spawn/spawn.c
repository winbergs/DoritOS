#include <aos/aos.h>
#include <spawn/spawn.h>

#include <elf/elf.h>
#include <aos/dispatcher_arch.h>
#include <barrelfish_kpi/paging_arm_v7.h>
#include <barrelfish_kpi/domain_params.h>
#include <spawn/multiboot.h>

extern struct bootinfo *bi;

struct process_info *process_list = NULL;

static size_t pid = 0;

static void add_parent_mapping(struct spawninfo *si, void *addr) {
    // Check if parent_mappings exists
    if (si->parent_mappings.addr == NULL) {
        si->parent_mappings.addr = addr;
    } else {
        // Iterate to last entry, allocate new mapping, assign
        struct parent_mapping *i;
        for(i = &si->parent_mappings; i->next != NULL; i = i->next);
        struct parent_mapping *mapping = (struct parent_mapping *) malloc(sizeof(struct parent_mapping));
        mapping->addr = addr;
        i->next = mapping;
    }
}

// Set up the cspace for a child process
static errval_t spawn_setup_cspace(struct spawninfo *si) {
    
    errval_t err;

    // Placeholder cnoderef/capref to reduce stack size
    struct cnoderef cnoderef_alpha;

    struct capref capref_alpha;
    struct capref capref_beta;

    // Create a L1 cnode
    err = cnode_create_l1(&si->child_rootcn_cap, &cnoderef_alpha);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Create L2 cnode: TASKCN
    err = cnode_create_foreign_l2(si->child_rootcn_cap, ROOTCN_SLOT_TASKCN, &si->taskcn_ref);
    if (err_is_fail(err)) {
        return err;
    }

    //  Create SLOT_DISPATCHER capability
    capref_alpha.cnode = si->taskcn_ref;
    capref_alpha.slot = TASKCN_SLOT_DISPATCHER;

    err = dispatcher_create(capref_alpha);
    if (err_is_fail(err)) {
        return err;
    }

    //  Copy the SLOT_DISPATCHER capability to parent cspace
    slot_alloc(&si->child_dispatcher_cap);
    err = cap_copy(si->child_dispatcher_cap, capref_alpha);
    if (err_is_fail(err)) {
        return err;
    }

    //  Retype SLOT_DISPATCHER capability into SLOT_SELFEP
    capref_beta.cnode = si->taskcn_ref;
    capref_beta.slot = TASKCN_SLOT_SELFEP;

    err = cap_retype(capref_beta, capref_alpha, 0, ObjType_EndPoint, 0, 1);
    if (err_is_fail(err)) {
        return err;
    }
    cap_delete(capref_alpha);

    //  Copy root cnode capability into SLOT_ROOTCN
    si->slot_rootcn_cap.cnode = si->taskcn_ref;
    si->slot_rootcn_cap.slot = TASKCN_SLOT_ROOTCN;
    cap_copy(si->slot_rootcn_cap, si->child_rootcn_cap);


    // Create L2 cnode: SLOT_ALLOC0
    err = cnode_create_foreign_l2(si->child_rootcn_cap, ROOTCN_SLOT_SLOT_ALLOC0, &cnoderef_alpha);
    if (err_is_fail(err)) {
        return err;
    }

    
    // Create L2 cnode: SLOT_ALLOC1
    err = cnode_create_foreign_l2(si->child_rootcn_cap, ROOTCN_SLOT_SLOT_ALLOC1, &cnoderef_alpha);
    if (err_is_fail(err)) {
        return err;
    }
    
    
    // Create L2 cnode: SLOT_ALLOC2
    err = cnode_create_foreign_l2(si->child_rootcn_cap, ROOTCN_SLOT_SLOT_ALLOC2, &cnoderef_alpha);
    if (err_is_fail(err)) {
        return err;
    }
    
    
    // Create L2 cnode: SLOT_BASE_PAGE_CN
    err = cnode_create_foreign_l2(si->child_rootcn_cap, ROOTCN_SLOT_BASE_PAGE_CN, &cnoderef_alpha);
    if (err_is_fail(err)) {
        return err;
    }
    
    //  Create RAM capabilities for SLOT_BASE_PAGE_CN
    err = ram_alloc(&capref_alpha, BASE_PAGE_SIZE * L2_CNODE_SLOTS);
    if (err_is_fail(err)) {
        return err;
    }

    capref_beta.cnode = cnoderef_alpha;
    capref_beta.slot = 0;

    err = cap_retype(capref_beta, capref_alpha, 0, ObjType_RAM, BASE_PAGE_SIZE, L2_CNODE_SLOTS);
    if (err_is_fail(err)) {
        return err;
    }

    cap_delete(capref_alpha);

    // Create L2 cnode: SLOT_PAGECN
    err = cnode_create_foreign_l2(si->child_rootcn_cap, ROOTCN_SLOT_PAGECN, &si->slot_pagecn_ref);
    if (err_is_fail(err)) {
        return err;
    }
    
    return SYS_ERR_OK;
    
}

// Set up the vspace for a child process
static errval_t spawn_setup_vspace(struct spawninfo *si) {
    
    errval_t err = SYS_ERR_OK;
    
    // Creating L1 VNode
    si->l1_pt_cap.cnode = si->slot_pagecn_ref;
    si->l1_pt_cap.slot = PAGECN_SLOT_VROOT;
    err = vnode_create(si->l1_pt_cap, ObjType_VNode_ARM_l1);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Copy capability to child's L1 pagetable into parent's cspace
    slot_alloc(&si->child_root_pt_cap);
    cap_copy(si->child_root_pt_cap, si->l1_pt_cap);
    
    // Initialize the child paging state
    err = paging_init_state(&si->child_paging_state,
                            // Subtract 128 pages for:
                            //  - DCB (64 pages)
                            //  - Argspace (1 page)
                            VADDR_OFFSET - 128 * BASE_PAGE_SIZE,
                            si->child_root_pt_cap,
                            get_default_slot_allocator());
    if (err_is_fail(err)) {
        return err;
    }
    
    return err;
    
}

static errval_t elf_allocator_callback(void *state, genvaddr_t base, size_t size, uint32_t flags, void **ret) {
    
    errval_t err = SYS_ERR_OK;
    struct spawninfo *si = (struct spawninfo *) state;

    // Aligning to page boundry in child's virtual memory
    genvaddr_t real_base = base / BASE_PAGE_SIZE;
    real_base *= BASE_PAGE_SIZE;
    size_t offset = base - real_base;

    // Allocating memory for section in ELF
    struct capref frame_cap;
    size_t ret_size;
    err = frame_alloc(&frame_cap, size + offset, &ret_size);
    if (err_is_fail(err)) {
        return err;
    }

    // Map the memory region into parent virtual address space
    err = paging_map_frame_attr(get_current_paging_state(), ret, ret_size, frame_cap, VREGION_FLAGS_READ_WRITE, NULL, NULL);
    if (err_is_fail(err)) {
        return err;
    }

    // Add mapping to parent mappings list
    add_parent_mapping(si, *ret);

    // Adding offset to target address
    *ret += offset;

    // Map the memory region into child's virtual address space.
    uint32_t real_base_addr = real_base;
    paging_alloc_fixed(&si->child_paging_state, (void *) real_base_addr, ret_size);
    err = paging_map_fixed_attr(&si->child_paging_state, real_base, frame_cap, ret_size, flags);
    if (err_is_fail(err)) {
        return err;
    }
    
    return err;
    
}

// Parse the ELF and copy the sections into memory
static errval_t spawn_parse_elf(struct spawninfo *si, void *elf, size_t elf_size) {
    
    errval_t err = SYS_ERR_OK;

    // Load the ELF
    err = elf_load(EM_ARM, &elf_allocator_callback, (void *) si, (lvaddr_t) elf, elf_size, &si->entry_addr);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Get the address of the .got section
    uint32_t elf_addr = (uint32_t) elf;
    struct Elf32_Shdr *got_header = elf32_find_section_header_name((genvaddr_t) elf_addr, elf_size, ".got");
    si->got_addr = (void *) got_header->sh_addr;
        
    return err;
    
}

// Set up the dispatcher
static errval_t spawn_setup_dispatcher(struct spawninfo *si) {
    
    errval_t err = SYS_ERR_OK;
    
    struct capref dcb_frame_cap;
    size_t dcb_size = 1 << DISPATCHER_FRAME_BITS;
    err = frame_alloc(&dcb_frame_cap, dcb_size, &dcb_size);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Map the memory into parent's virtual address space
    err = paging_map_frame_attr(get_current_paging_state(), &si->dcb_addr_parent, dcb_size, dcb_frame_cap, VREGION_FLAGS_READ_WRITE, NULL, NULL);
    if (err_is_fail(err)) {
        return err;
    }

    // Add mapping to parent mappings list
    add_parent_mapping(si, si->dcb_addr_parent);
    
    // Map the memory region into child's virtual address space.
    void *dcb_addr_child;
    err = paging_map_frame_attr(&si->child_paging_state, &dcb_addr_child, dcb_size, dcb_frame_cap, VREGION_FLAGS_READ_WRITE, NULL, NULL);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Copy dcb frame capability into SLOT_DISPFRAME
    si->slot_dispframe_cap.cnode = si->taskcn_ref;
    si->slot_dispframe_cap.slot = TASKCN_SLOT_DISPFRAME;
    cap_copy(si->slot_dispframe_cap, dcb_frame_cap);

    // Get references to dispatcher structs
    dispatcher_handle_t dcb_addr_parent_handle = (dispatcher_handle_t) si->dcb_addr_parent;
    struct dispatcher_shared_generic *disp = get_dispatcher_shared_generic(dcb_addr_parent_handle);
    struct dispatcher_generic *disp_gen = get_dispatcher_generic(dcb_addr_parent_handle);
    struct dispatcher_shared_arm *disp_arm = get_dispatcher_shared_arm(dcb_addr_parent_handle);
    arch_registers_state_t *enabled_area = dispatcher_get_enabled_save_area(dcb_addr_parent_handle);
    arch_registers_state_t *disabled_area = dispatcher_get_disabled_save_area(dcb_addr_parent_handle);
    
    // Set dispatcher information
    disp_gen->core_id = 0;  // TODO: Core id of the dispatcher
    disp->udisp = (lvaddr_t) dcb_addr_child;   // Address of dispatcher frame in child's vspace
    disp->disabled = 1;     // Start the dispatcher in disabled mode
    disp->fpu_trap = 1;     // Trap on fpr instructions
    strncpy(disp->name, si->binary_name, DISP_NAME_LEN);    // Name of dispatcher
    
    // Set address of first instruction
    disabled_area->named.pc = si->entry_addr;
    
    // Initialize offest register
    disp_arm->got_base = (lvaddr_t) si->got_addr;
    enabled_area->regs[REG_OFFSET(PIC_REGISTER)] = (lvaddr_t) si->got_addr;
    disabled_area->regs[REG_OFFSET(PIC_REGISTER)] = (lvaddr_t) si->got_addr;

    enabled_area->named.cpsr = CPSR_F_MASK | ARM_MODE_USR;
    disabled_area->named.cpsr = CPSR_F_MASK | ARM_MODE_USR;
    disp_gen->eh_frame = 0;
    disp_gen->eh_frame_size = 0;
    disp_gen->eh_frame_hdr = 0;
    disp_gen->eh_frame_hdr_size = 0;
    
    return err;
    
}

static errval_t spawn_setup_args(struct spawninfo *si, const char *argstring) {

    errval_t err = SYS_ERR_OK;
    
    struct capref argspace_frame_cap;
    size_t size = BASE_PAGE_SIZE;
    err = frame_alloc(&argspace_frame_cap, size, &size);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Map the memory into parent's virtual address space
    void *argspace_addr_parent;
    err = paging_map_frame_attr(get_current_paging_state(), &argspace_addr_parent, size, argspace_frame_cap, VREGION_FLAGS_READ_WRITE, NULL, NULL);
    if (err_is_fail(err)) {
        return err;
    }

    // Add mapping to parent mappings list
    add_parent_mapping(si, argspace_addr_parent);
    
    // Map the memory region into child's virtual address space.
    void *argspace_addr_child;
    err = paging_map_frame_attr(&si->child_paging_state, &argspace_addr_child, size, argspace_frame_cap, VREGION_FLAGS_READ_WRITE, NULL, NULL);
    if (err_is_fail(err)) {
        return err;
    }
    
    // Copy argspace frame capability into SLOT_ARGSPAGE
    struct capref slot_argspace_cap = {
        .cnode = si->taskcn_ref,
        .slot = TASKCN_SLOT_ARGSPAGE
    };
    cap_copy(slot_argspace_cap, argspace_frame_cap);
    
    // Zero out the argspace
    memset(argspace_addr_parent, 0, BASE_PAGE_SIZE);
    
    // Get a reference to the spawn_domain_params struct
    struct spawn_domain_params *params = (struct spawn_domain_params *) argspace_addr_parent;
    
    void *argspace_offset = argspace_addr_parent + sizeof(struct spawn_domain_params);
    
    // Get length of argstring
    size_t len = strlen(argstring);
    
    // Allocate args
    char *args = (char *) argspace_offset;
    argspace_offset += len;

    // Initialization
    params->argc = 0;
    params->argv[params->argc++] = args;


    // Copy argstring to args
    uint8_t escaped = 0;
    char quote = '\0';
    size_t j = 0;
    for (size_t i = 0; i < len && params->argc < MAX_CMDLINE_ARGS; i++) {
        // If escaped, copy and reset escape flag
        if (escaped) {
            args[j++] = argstring[i];
            escaped = !escaped;
        } else {
            // If quote is null
            if (quote == '\0') {
                // Check if quote or space or escape, else copy
                if (argstring[i] == '"' || argstring[i] == '\'') {
                    quote = argstring[i];
                } else if(argstring[i] == ' ') {
                    args[j++] = '\0';
                    params->argv[params->argc++] = args + j;
                } else if (argstring[i] == '\\') {
                    escaped = !escaped;
                } else {
                    args[j++] = argstring[i];
                }
            } else {
                // Check if matching quote, else copy
                if (argstring[i] == quote) {
                    quote = '\0';
                } else {
                    args[j++] = argstring[i];
                }
            }
        }
    }

    // Terminate argv
    params->argv[params->argc] = NULL;
    
    // Terminate envp
    params->envp[0] = NULL;
    
    // Set argument address in register r0 in enabled state of child
    dispatcher_handle_t dcb_addr_parent_handle = (dispatcher_handle_t) si->dcb_addr_parent;
    arch_registers_state_t *enabled_area = dispatcher_get_enabled_save_area(dcb_addr_parent_handle);
    enabled_area->named.r0 = (uint32_t) argspace_addr_child;

    return err;
    
}

static errval_t spawn_invoke_dispatcher(struct spawninfo *si) {
 
    errval_t err = SYS_ERR_OK;

    struct process_info *process = malloc(sizeof(struct process_info));

    process->name = si->binary_name;
    process->dispatcher_cap = &si->child_dispatcher_cap;
    process->id = ++pid;

    if (process_list == NULL) {
        process_list = process;
    } else {
        struct process_info *i;
        for(i = process_list; i->next != NULL; i = i->next);
        i->next = process;
        process->prev = i;
    }

    err = invoke_dispatcher(si->child_dispatcher_cap,
                            cap_dispatcher,
                            si->child_rootcn_cap,
                            si->l1_pt_cap,
                            si->slot_dispframe_cap,
                            true);

    return err;
    
}

static errval_t spawn_cleanup(struct spawninfo *si) {

    errval_t err = SYS_ERR_OK;

    struct parent_mapping *i;
    for (i = &si->parent_mappings; i != NULL; i = i->next) {
        err = paging_unmap(get_current_paging_state(), i->addr);
        if (err_is_fail(err)) {
            return err;
        }
    }

    cap_delete(si->child_rootcn_cap);
    cap_delete(si->child_root_pt_cap);

    return err;
}

// TODO(M2): Implement this function such that it starts a new process
// TODO(M4): Build and pass a messaging channel to your child process
errval_t spawn_load_by_name(void * binary_name, struct spawninfo * si) {
    printf("spawn start_child: starting: %s\n", binary_name);

    errval_t err = SYS_ERR_OK;

    // Init spawninfo
    memset(si, 0, sizeof(*si));
    si->binary_name = binary_name;

    // TODO: Implement me
    // - Get the binary from multiboot image
    // - Map multiboot module in your address space
    // - Setup childs cspace
    // - Setup childs vspace
    // - Load the ELF binary
    // - Setup dispatcher
    // - Setup environment
    // - Make dispatcher runnable

    // Finding the memory region containing the ELF image
    struct mem_region *mem = multiboot_find_module(bi, si->binary_name);
    if (!mem) {
        return SPAWN_ERR_FIND_MODULE;
    }

    // Constructing the capability for the frame containing the ELF image
    struct capref child_frame = {
        .cnode = cnode_module,
        .slot = mem->mrmod_slot
    };

    // Mapping the ELF image into the virtual address space
    void *elf_buf = NULL;
    err = paging_map_frame_attr(get_current_paging_state(), &elf_buf, mem->mrmod_size, child_frame, VREGION_FLAGS_READ, NULL, NULL);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed mapping ELF into virtual memory: %s\n", err_getstring(err));
        return err;
    }

    // Add mapping to parent mappings list
    add_parent_mapping(si, elf_buf);

    char *elf = elf_buf;
    debug_printf("Mapped ELF into memory: 0x%x %c%c%c\n", elf[0], elf[1], elf[2], elf[3]);
    assert(elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F');

    // Set up cspace
    err = spawn_setup_cspace(si);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed setting up cspace: %s\n", err_getstring(err));
        return err;
    }
    
    // Set up vspace
    err = spawn_setup_vspace(si);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed setting up vspace: %s\n", err_getstring(err));
        return err;
    }
    
    // Parse the elf
    err = spawn_parse_elf(si, elf_buf, mem->mrmod_size);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed to parse the ELF: %s\n", err_getstring(err));
        return err;
    }
    
    // Set up the child's dispatcher
    err = spawn_setup_dispatcher(si);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed setting up the dispatcher: %s\n", err_getstring(err));
        return err;
    }
    
    // Get arguments string from multiboot
    const char *argstring = multiboot_module_opts(mem);
    
    // Set up the arguments for the child process
    err = spawn_setup_args(si, argstring);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed setting up the arguments: %s\n", err_getstring(err));
        return err;
    }
    
    // Launch dispatcher 🚀
    err = spawn_invoke_dispatcher(si);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed invoking the dispatcher: %s\n", err_getstring(err));
        return err;
    }

    // Cleanup 
    err = spawn_cleanup(si);
    if (err_is_fail(err)) {
        debug_printf("spawn: Failed cleanup: %s\n", err_getstring(err));
        return err;
    }

    return err;
}

void print_process_list(void) {
    size_t counter = 1;

    struct process_info *i;
    debug_printf("Currently running processes:\n");
    debug_printf("\t%3d\t%s\n", 0, "init");
    for (i = process_list; i != NULL; i = i->next) {
        debug_printf("\t%3d\t%s\n", i->id, i->name);
        counter++;
    }
    debug_printf("Total number of processes: %d\n", counter);
}
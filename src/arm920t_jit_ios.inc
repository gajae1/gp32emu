/* iOS developer JIT: the debugger initializes RX pages before this instance
 * starts emitting through a shared RW alias. Neither mmap success nor debugger
 * attachment alone proves that anonymous pages can execute on iOS. */
#define ARM_JIT_IOS_MARKER 0xd4200000u /* BRK #0, never executed during setup. */

static int arm_jit_ios_rx_valid(const arm920t_t *c) {
    vm_address_t cursor = (vm_address_t)c->jit_ios_rx;
    vm_address_t end = cursor + c->jit_code_size;
    while (cursor < end) {
        vm_address_t base = cursor;
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = vm_region_64(mach_task_self(), &base, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || base > cursor || !size || base + size <= cursor ||
            (info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) !=
                (VM_PROT_READ | VM_PROT_EXECUTE) || (info.protection & VM_PROT_WRITE))
            return 0;
        cursor = base + size;
    }
    return 1;
}

int arm920t_prepare_ios_jit(arm920t_t *c, gp32_ios_jit_region_t *region) {
    if (region) memset(region, 0, sizeof(*region));
    if (!c || !region || c->running || c->jit_ios_ready) return 0;
    if (!c->jit_code) {
        size_t size = ARM_JIT_CODE_SIZE;
        void *original = mmap(NULL, size, PROT_READ | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (original == MAP_FAILED) return 0;
        vm_address_t alias = 0;
        vm_prot_t current = 0, maximum = 0;
        kern_return_t kr = vm_remap(mach_task_self(), &alias, size, 0,
            VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)original, FALSE,
            &current, &maximum, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) { munmap(original, size); return 0; }
        c->jit_code = original;
        c->jit_ios_rx = (uint8_t *)alias;
        c->jit_code_size = size;
        if (!arm_jit_ios_rx_valid(c)) {
            arm_jit_free_exec(c, c->jit_code, size);
            c->jit_code = NULL;
            c->jit_code_size = 0;
            return 0;
        }
    }
    region->execute = c->jit_ios_rx;
    region->size = c->jit_code_size;
    region->page_size = (size_t)vm_page_size;
    region->marker = ARM_JIT_IOS_MARKER;
    return 1;
}

static int arm_jit_ios_activate(arm920t_t *c, uint32_t marker) {
    if (!c || c->running || c->jit_ios_ready || !c->jit_code || !c->jit_ios_rx)
        return 0;
    if (!arm_jit_ios_rx_valid(c)) return 0;
    /* A read-only prepared arena starts zeroed. Check both views on EVERY
     * host page: incomplete debugger initialization or a private COW remap
     * must never lead to executing an unprepared page. LLDB initializes the
     * whole region; universal.js writes 0x69 at each 16 KiB page boundary. */
    size_t page = (size_t)vm_page_size;
    if (!page || c->jit_code_size % page) return 0;
    if (marker == 0x69u && page != 16384u) return 0;
    for (size_t off = 0; off < c->jit_code_size; off += page) {
        uint32_t rx, original;
        memcpy(&rx, c->jit_ios_rx + off, sizeof(rx));
        memcpy(&original, c->jit_code + off, sizeof(original));
        if (rx != marker || original != rx) return 0;
    }
    if (vm_protect(mach_task_self(), (vm_address_t)c->jit_code, c->jit_code_size,
                   FALSE, VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) return 0;
    /* The original view must now remain shared with the RX view. */
    uint32_t changed = 0;
    memcpy(c->jit_code, &changed, sizeof(changed));
    uint32_t observed;
    memcpy(&observed, c->jit_ios_rx, sizeof(observed));
    if (observed == changed && arm_jit_ios_rx_valid(c)) {
        c->jit_ios_ready = 1;
        arm920t_jit_invalidate_all(c, ARM_JIT_INV_API_FLUSH);
        if (a64_emit_runtime(c)) {
            arm920t_set_jit(c, 1);
            if (c->jit_enabled) return 1;
        }
    }
    arm920t_set_jit(c, 0);
    arm_jit_free_exec(c, c->jit_code, c->jit_code_size);
    c->jit_code = NULL;
    c->jit_code_size = 0;
    return 0;
}

int arm920t_activate_ios_jit(arm920t_t *c) {
    return arm_jit_ios_activate(c, ARM_JIT_IOS_MARKER);
}

int arm920t_activate_ios_jit_universal(arm920t_t *c) {
    return arm_jit_ios_activate(c, 0x69u);
}

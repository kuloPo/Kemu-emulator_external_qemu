/* Copyright 2026 The Android Open Source Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

static unsigned pipe_tx_mapped_pages;

static void pipe_tx_runstate(void* opaque, int running, RunState state) {
    HwPipe* pipe = opaque;
    android_pipe_tx_set_running(pipe->host_pipe, running);
}

static PipeTxMapping* pipe_tx_new(void) {
    PipeTxMapping* tx = g_new0(PipeTxMapping, 1);
    tx->seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    return tx;
}

static void* pipe_tx_map_page(PipeTxMapping* tx, uint64_t address, bool writable) {
    if ((address & (PIPE_TX_PAGE_SIZE - 1)) ||
        pipe_tx_mapped_pages >= PIPE_TX_MAX_PAGES ||
        g_hash_table_contains(tx->seen, &address)) return NULL;
    void* page = map_guest_buffer(address, PIPE_TX_PAGE_SIZE, writable);
    if (!page) return NULL;
    if (qemu_ram_addr_from_host(page) == RAM_ADDR_INVALID) {
        cpu_physical_memory_unmap(page, PIPE_TX_PAGE_SIZE, writable, 0);
        return NULL;
    }
    uint64_t* key = g_new(uint64_t, 1);
    *key = address;
    g_hash_table_add(tx->seen, key);
    ++pipe_tx_mapped_pages;
    return page;
}

static bool pipe_tx_add_page(PipeTxMapping* tx, unsigned id, uint64_t address) {
    if (id >= PIPE_TX_MAX_SEGMENTS) return false;
    void* page = pipe_tx_map_page(tx, address, false);
    if (!page) return false;
    if (!tx->pages[id]) {
        tx->pages[id] = g_ptr_array_new();
        tx->addresses[id] = g_array_new(false, false, sizeof(uint64_t));
    }
    g_ptr_array_add(tx->pages[id], page);
    g_array_append_val(tx->addresses[id], address);
    return true;
}

static void pipe_tx_free(HwPipe* pipe) {
    PipeTxMapping* tx = pipe->tx;
    if (!tx) return;
    if (tx->state_handler) qemu_del_vm_change_state_handler(tx->state_handler);
    for (unsigned i = 0; i < tx->control_count; ++i) {
        cpu_physical_memory_unmap(tx->control[i], PIPE_TX_PAGE_SIZE, i == 1,
                                  PIPE_TX_PAGE_SIZE);
        --pipe_tx_mapped_pages;
    }
    for (unsigned id = 0; id < PIPE_TX_MAX_SEGMENTS; ++id) {
        if (!tx->pages[id]) continue;
        for (unsigned i = 0; i < tx->pages[id]->len; ++i) {
            cpu_physical_memory_unmap(tx->pages[id]->pdata[i], PIPE_TX_PAGE_SIZE, false, 0);
            --pipe_tx_mapped_pages;
        }
        g_ptr_array_free(tx->pages[id], true);
        g_array_free(tx->addresses[id], true);
    }
    g_hash_table_destroy(tx->seen);
    g_free(tx);
    pipe->tx = NULL;
}

static int pipe_tx_attach(HwPipe* pipe, bool restoring) {
    PipeTxMapping* tx = pipe->tx;
    if (tx->control_count != 2) return -1;
    if (android_pipe_tx_setup(pipe->host_pipe, tx->control[0], tx->control[1],
                             pipe_rx_dirty, restoring, rcu_register_thread,
                             rcu_unregister_thread)) return -1;
    tx->state_handler = qemu_add_vm_change_state_handler(pipe_tx_runstate, pipe);
    android_pipe_tx_set_running(pipe->host_pipe, runstate_is_running());
    for (unsigned id = 0; id < tx->segments; ++id) {
        if (!tx->pages[id] || android_pipe_tx_add_segment(pipe->host_pipe, id,
                tx->pages[id]->pdata, tx->pages[id]->len)) return -1;
    }
    return tx->active ? android_pipe_tx_activate(pipe->host_pipe) : 0;
}

static void pipe_tx_command(HwPipe* pipe, unsigned command) {
    PipeCommand* cb = pipe->command_buffer;
    cb->status = GOLDFISH_PIPE_ERROR_INVAL;
    if (command == PIPE_CMD_TX_SETUP) {
        if (pipe->tx || !pipe->rx || !pipe->rx->active ||
            cb->rw_params.buffers_count != 2) return;
        pipe->tx = pipe_tx_new();
        uint64_t* addresses = hwpipe_get_command_rw_ptrs(pipe);
        uint32_t* sizes = hwpipe_get_command_rw_sizes(pipe);
        for (unsigned i = 0; i < 2; ++i) {
            if (sizes[i] != PIPE_TX_PAGE_SIZE) return;
            void* page = pipe_tx_map_page(pipe->tx, addresses[i], i == 1);
            if (!page) return;
            pipe->tx->control[i] = page;
            pipe->tx->control_addresses[i] = addresses[i];
            ++pipe->tx->control_count;
        }
        if (pipe_tx_attach(pipe, false)) return;
    } else {
        PipeTxMapping* tx = pipe->tx;
        if (!tx || tx->control_count != 2) return;
        const unsigned id = cb->rw_params.consumed_size;
        switch (command) {
        case PIPE_CMD_TX_ADD_PAGES: {
            const unsigned count = cb->rw_params.buffers_count;
            if (id != tx->segments || id >= PIPE_TX_MAX_SEGMENTS || !count ||
                count > pipe->rw_params_max_count ||
                count > (COMMAND_BUFFER_SIZE - 24U) / 12U) return;
            uint64_t* addresses = hwpipe_get_command_rw_ptrs(pipe);
            uint32_t* sizes = hwpipe_get_command_rw_sizes(pipe);
            for (unsigned i = 0; i < count; ++i) {
                if (sizes[i] != PIPE_TX_PAGE_SIZE ||
                    !pipe_tx_add_page(tx, id, addresses[i])) return;
            }
            break;
        }
        case PIPE_CMD_TX_FINISH_SEGMENT:
            if (id != tx->segments || id >= PIPE_TX_MAX_SEGMENTS ||
                !tx->pages[id] || !tx->pages[id]->len ||
                android_pipe_tx_add_segment(pipe->host_pipe, id,
                        tx->pages[id]->pdata, tx->pages[id]->len)) return;
            ++tx->segments;
            break;
        case PIPE_CMD_TX_ACTIVATE:
            if (tx->active || tx->segments < 2 || android_pipe_tx_activate(pipe->host_pipe)) return;
            tx->active = true;
            break;
        case PIPE_CMD_TX_KICK:
            if (!tx->active) return;
            android_pipe_tx_kick(pipe->host_pipe);
            break;
        case PIPE_CMD_TX_ARM:
            if (!tx->active) return;
            android_pipe_tx_arm(pipe->host_pipe, cb->rw_params.buffers_count);
            break;
        default:
            return;
        }
    }
    cb->status = 0;
}

static void pipe_tx_save(HwPipe* pipe, QEMUFile* file) {
    PipeTxMapping* tx = pipe->tx;
    qemu_put_byte(file, tx != NULL);
    if (!tx) return;
    qemu_put_byte(file, tx->active);
    qemu_put_be32(file, tx->segments);
    qemu_put_be32(file, tx->control_count);
    for (unsigned i = 0; i < tx->control_count; ++i)
        qemu_put_be64(file, tx->control_addresses[i]);
    for (unsigned id = 0; id < PIPE_TX_MAX_SEGMENTS; ++id) {
        const unsigned count = tx->pages[id] ? tx->pages[id]->len : 0;
        qemu_put_be32(file, count);
        for (unsigned i = 0; i < count; ++i)
            qemu_put_be64(file, g_array_index(tx->addresses[id], uint64_t, i));
    }
}

static bool pipe_tx_load(HwPipe* pipe, QEMUFile* file) {
    if (!qemu_get_byte(file)) return true;
    pipe->tx = pipe_tx_new();
    PipeTxMapping* tx = pipe->tx;
    tx->active = qemu_get_byte(file);
    tx->segments = qemu_get_be32(file);
    const unsigned controls = qemu_get_be32(file);
    if (tx->segments > PIPE_TX_MAX_SEGMENTS || controls > 2 ||
        (tx->active && (controls != 2 || tx->segments < 2))) return false;
    for (unsigned i = 0; i < controls; ++i) {
        const uint64_t address = qemu_get_be64(file);
        void* page = pipe_tx_map_page(tx, address, i == 1);
        if (!page) return false;
        tx->control[i] = page;
        tx->control_addresses[i] = address;
        ++tx->control_count;
    }
    for (unsigned id = 0; id < PIPE_TX_MAX_SEGMENTS; ++id) {
        const unsigned count = qemu_get_be32(file);
        if (count > PIPE_TX_MAX_PAGES || (id < tx->segments && !count) ||
            (id > tx->segments && count)) return false;
        for (unsigned i = 0; i < count; ++i)
            if (!pipe_tx_add_page(tx, id, qemu_get_be64(file))) return false;
    }
    return true;
}

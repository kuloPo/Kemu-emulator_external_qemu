/*
 * WHPX platform APIC support
 *
 * Copyright (c) 2011 Siemens AG
 *
 * Authors:
 *  Jan Kiszka          <jan.kiszka@siemens.com>
 *  John Starks         <jostarks@microsoft.com>
 *
 * This work is licensed under the terms of the GNU GPL version 2.
 * See the COPYING file in the top-level directory.
 */
#include "whp-dispatch.h"
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "hw/i386/apic_internal.h"
#include "hw/i386/apic-msidef.h"
#include "hw/pci/msi.h"
#include "sysemu/whpx.h"
#include "whpx-internal.h"

/* The xAPIC register page: one 32-bit register every 16 bytes. */
struct whpx_lapic_state {
    struct {
        uint32_t data;
        uint32_t padding[3];
    } fields[256];
};

static HRESULT whpx_get_lapic(CPUState *cpu, struct whpx_lapic_state *kapic)
{
    UINT32 written = 0;

    if (whp_dispatch.WHvGetVirtualProcessorState) {
        return whp_dispatch.WHvGetVirtualProcessorState(
            whpx_global.partition, cpu->cpu_index,
            WHvVirtualProcessorStateTypeInterruptControllerState2,
            kapic, sizeof(*kapic), &written);
    }
    return whp_dispatch.WHvGetVirtualProcessorInterruptControllerState2(
        whpx_global.partition, cpu->cpu_index, kapic, sizeof(*kapic),
        &written);
}

static HRESULT whpx_set_lapic(CPUState *cpu, struct whpx_lapic_state *kapic)
{
    if (whp_dispatch.WHvSetVirtualProcessorState) {
        return whp_dispatch.WHvSetVirtualProcessorState(
            whpx_global.partition, cpu->cpu_index,
            WHvVirtualProcessorStateTypeInterruptControllerState2,
            kapic, sizeof(*kapic));
    }
    return whp_dispatch.WHvSetVirtualProcessorInterruptControllerState2(
        whpx_global.partition, cpu->cpu_index, kapic, sizeof(*kapic));
}

/*
 * Only the registers that QEMU models are written; the rest of the page
 * (version, current timer count, ...) keeps the hypervisor's values.
 */
static void whpx_put_apic_state(APICCommonState *s,
                                struct whpx_lapic_state *kapic)
{
    int i;

    kapic->fields[0x2].data = s->id << 24;
    kapic->fields[0x8].data = s->tpr;
    kapic->fields[0xd].data = s->log_dest << 24;
    kapic->fields[0xe].data = (uint32_t)s->dest_mode << 28 | 0x0fffffff;
    kapic->fields[0xf].data = s->spurious_vec;
    for (i = 0; i < 8; i++) {
        kapic->fields[0x10 + i].data = s->isr[i];
        kapic->fields[0x18 + i].data = s->tmr[i];
        kapic->fields[0x20 + i].data = s->irr[i];
    }

    kapic->fields[0x28].data = s->esr;
    kapic->fields[0x30].data = s->icr[0];
    kapic->fields[0x31].data = s->icr[1];
    for (i = 0; i < APIC_LVT_NB; i++) {
        kapic->fields[0x32 + i].data = s->lvt[i];
    }

    kapic->fields[0x38].data = s->initial_count;
    kapic->fields[0x3e].data = s->divide_conf;
}

static void whpx_get_apic_state(APICCommonState *s,
                                struct whpx_lapic_state *kapic)
{
    int i, v;

    s->id = kapic->fields[0x2].data >> 24;
    s->tpr = kapic->fields[0x8].data;
    s->arb_id = kapic->fields[0x9].data;
    s->log_dest = kapic->fields[0xd].data >> 24;
    s->dest_mode = kapic->fields[0xe].data >> 28;
    s->spurious_vec = kapic->fields[0xf].data;
    for (i = 0; i < 8; i++) {
        s->isr[i] = kapic->fields[0x10 + i].data;
        s->tmr[i] = kapic->fields[0x18 + i].data;
        s->irr[i] = kapic->fields[0x20 + i].data;
    }

    s->esr = kapic->fields[0x28].data;
    s->icr[0] = kapic->fields[0x30].data;
    s->icr[1] = kapic->fields[0x31].data;
    for (i = 0; i < APIC_LVT_NB; i++) {
        s->lvt[i] = kapic->fields[0x32 + i].data;
    }

    s->initial_count = kapic->fields[0x38].data;
    s->divide_conf = kapic->fields[0x3e].data;

    v = (s->divide_conf & 3) | ((s->divide_conf >> 1) & 4);
    s->count_shift = (v + 1) & 7;

    s->initial_count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    apic_next_timer(s, s->initial_count_load_time);
}

static void whpx_put_apic_base(CPUState *cpu, uint64_t val)
{
    HRESULT hr;
    WHV_REGISTER_VALUE reg_value = {.Reg64 = val};
    WHV_REGISTER_NAME reg_name = WHvX64RegisterApicBase;

    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(
        whpx_global.partition, cpu->cpu_index, &reg_name, 1, &reg_value);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set MSR APIC base, hr=%08lx", hr);
    }
}

static void whpx_apic_put(CPUState *cs, run_on_cpu_data data)
{
    APICCommonState *s = data.host_ptr;
    struct whpx_lapic_state kapic;
    bool startup_suspend, halt_suspend;
    HRESULT hr;

    whpx_put_apic_base(cs, s->apicbase);

    /*
     * Hyper-V does not apply an APIC state to a vCPU that it holds halted;
     * the call waits for the vCPU to run, which its own thread cannot do.
     * An INIT takes the vCPU out of the halt first.
     */
    if (whpx_get_activity_state(cs, &startup_suspend, &halt_suspend) &&
        halt_suspend) {
        WHV_INTERRUPT_CONTROL init;

        memset(&init, 0, sizeof(init));
        init.Type = WHvX64InterruptTypeInit;
        init.DestinationMode = WHvX64InterruptDestinationModePhysical;
        init.TriggerMode = WHvX64InterruptTriggerModeEdge;
        init.Destination = s->id;
        hr = whp_dispatch.WHvRequestInterrupt(whpx_global.partition, &init,
                                              sizeof(init));
        if (FAILED(hr)) {
            error_report("WHPX: Failed to INIT halted vCPU %d, hr=%08lx",
                         cs->cpu_index, hr);
        }
    }

    hr = whpx_get_lapic(cs, &kapic);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get APIC state, hr=%08lx", hr);
        abort();
    }
    whpx_put_apic_state(s, &kapic);
    hr = whpx_set_lapic(cs, &kapic);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to set APIC state, hr=%08lx", hr);
        abort();
    }

    /* The hypervisor keeps the application processors waiting for SIPI. */
    whpx_set_activity_state(cs, s->wait_for_sipi, false);
}

void whpx_apic_get(DeviceState *dev)
{
    APICCommonState *s = APIC_COMMON(dev);
    CPUState *cpu = CPU(s->cpu);
    struct whpx_lapic_state kapic;
    bool startup_suspend, halt_suspend;
    HRESULT hr;

    hr = whpx_get_lapic(cpu, &kapic);
    if (FAILED(hr)) {
        error_report("WHPX: Failed to get APIC state, hr=%08lx", hr);
        abort();
    }

    whpx_get_apic_state(s, &kapic);
    if (whpx_get_activity_state(cpu, &startup_suspend, &halt_suspend)) {
        s->wait_for_sipi = startup_suspend;
    }
}

static void whpx_apic_set_base(APICCommonState *s, uint64_t val)
{
    s->apicbase = val;
}

static void whpx_apic_set_tpr(APICCommonState *s, uint8_t val)
{
    s->tpr = (val & 0x0f) << 4;
}

static uint8_t whpx_apic_get_tpr(APICCommonState *s)
{
    return s->tpr >> 4;
}

static void whpx_apic_vapic_base_update(APICCommonState *s)
{
    /* The TPR patching VAPIC is not used with the hypervisor APIC. */
}

static void whpx_apic_external_nmi(APICCommonState *s)
{
    cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_NMI);
}

static void whpx_send_msi(MSIMessage *msg)
{
    uint64_t addr = msg->address;
    uint32_t data = msg->data;
    uint8_t dest = (addr & MSI_ADDR_DEST_ID_MASK) >> MSI_ADDR_DEST_ID_SHIFT;
    uint8_t vector = (data & MSI_DATA_VECTOR_MASK) >> MSI_DATA_VECTOR_SHIFT;
    uint8_t dest_mode = (addr >> MSI_ADDR_DEST_MODE_SHIFT) & 0x1;
    uint8_t trigger_mode = (data >> MSI_DATA_TRIGGER_SHIFT) & 0x1;
    uint8_t delivery = (data >> MSI_DATA_DELIVERY_MODE_SHIFT) & 0x7;
    WHV_INTERRUPT_CONTROL interrupt;
    HRESULT hr;

    /*
     * An IOAPIC pin in ExtINT mode forwards the 8259 output, which
     * pic_irq_request() already injects as an external interrupt.  Vectors
     * below 16 are illegal and dropped, like an APIC does.
     */
    if (delivery == APIC_DM_EXTINT ||
        (vector < 16 && delivery != APIC_DM_NMI && delivery != APIC_DM_INIT)) {
        return;
    }

    memset(&interrupt, 0, sizeof(interrupt));
    /* The interrupt types match the MSI delivery modes. */
    interrupt.Type = delivery;
    interrupt.DestinationMode = dest_mode ?
        WHvX64InterruptDestinationModeLogical :
        WHvX64InterruptDestinationModePhysical;
    interrupt.TriggerMode = trigger_mode ?
        WHvX64InterruptTriggerModeLevel : WHvX64InterruptTriggerModeEdge;
    interrupt.Vector = vector;
    interrupt.Destination = dest;

    whpx_stats_count_request_irq();
    hr = whp_dispatch.WHvRequestInterrupt(whpx_global.partition, &interrupt,
                                          sizeof(interrupt));
    if (FAILED(hr)) {
        error_report("WHPX: MSI (%" PRIx64 ", %x) delivery %d dest_mode %d"
                     " trigger %d vector %d lost, hr=%08lx",
                     addr, data, delivery, dest_mode, trigger_mode, vector,
                     hr);
    }
}

static uint64_t whpx_apic_mem_read(void *opaque, hwaddr addr,
                                   unsigned size)
{
    return ~(uint64_t)0;
}

static void whpx_apic_mem_write(void *opaque, hwaddr addr,
                                uint64_t data, unsigned size)
{
    MSIMessage msg = { .address = addr, .data = data };

    whpx_send_msi(&msg);
}

static const MemoryRegionOps whpx_apic_io_ops = {
    .read = whpx_apic_mem_read,
    .write = whpx_apic_mem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void whpx_apic_reset(APICCommonState *s)
{
    run_on_cpu(CPU(s->cpu), whpx_apic_put, RUN_ON_CPU_HOST_PTR(s));
}

static void whpx_apic_post_load(APICCommonState *s)
{
    run_on_cpu(CPU(s->cpu), whpx_apic_put, RUN_ON_CPU_HOST_PTR(s));
}

static void whpx_apic_realize(DeviceState *dev, Error **errp)
{
    APICCommonState *s = APIC_COMMON(dev);

    memory_region_init_io(&s->io_memory, OBJECT(s), &whpx_apic_io_ops, s,
                          "whpx-apic-msi", APIC_SPACE_SIZE);

    msi_nonbroken = true;
}

static void whpx_apic_unrealize(DeviceState *dev, Error **errp)
{
}

static void whpx_apic_class_init(ObjectClass *klass, void *data)
{
    APICCommonClass *k = APIC_COMMON_CLASS(klass);

    k->realize = whpx_apic_realize;
    k->unrealize = whpx_apic_unrealize;
    k->reset = whpx_apic_reset;
    k->set_base = whpx_apic_set_base;
    k->set_tpr = whpx_apic_set_tpr;
    k->get_tpr = whpx_apic_get_tpr;
    k->post_load = whpx_apic_post_load;
    k->vapic_base_update = whpx_apic_vapic_base_update;
    k->external_nmi = whpx_apic_external_nmi;
    k->send_msi = whpx_send_msi;
}

static const TypeInfo whpx_apic_info = {
    .name = "whpx-apic",
    .parent = TYPE_APIC_COMMON,
    .instance_size = sizeof(APICCommonState),
    .class_init = whpx_apic_class_init,
};

static void whpx_apic_register_types(void)
{
    type_register_static(&whpx_apic_info);
}

type_init(whpx_apic_register_types)

#include "oscos/utils/save-ctx.S"

.macro default_vt_entry vten label_name
\label_name:
    save_aapcs
    mov x0, \vten
    bl xcpt_default_handler
    load_aapcs
    eret

.type \label_name, function
.size \label_name, . - \label_name

.align 7
.endm

.section ".text"

.align 11
xcpt_vector_table:
    // Exception from the current EL while using SP_EL0.

    // Synchronous.
    default_vt_entry 0x0 xcpt_sync_curr_el_sp_el0_handler

    // IRQ.
    default_vt_entry 0x1 xcpt_irq_curr_el_sp_el0_handler

    // FIQ.
    default_vt_entry 0x2 xcpt_fiq_curr_el_sp_el0_handler

    // SError.
    default_vt_entry 0x3 xcpt_serror_curr_el_sp_el0_handler

    // Exception from the current EL while using SP_ELx.

    // Synchronous.
xcpt_sync_curr_el_sp_elx_handler:
    save_aapcs

    // Check if the exception is caused by an instruction abort without a change
    // in exception level.
    mrs x0, esr_el1
    ubfx x1, x0, 26, 6
    cmp x1, 0x21
    b.ne .Lxcpt_sync_curr_el_sp_elx_handler_not_insn_abort

    bl xcpt_insn_abort_handler
    b .Lxcpt_sync_curr_el_sp_elx_handler_end

.Lxcpt_sync_curr_el_sp_elx_handler_not_insn_abort:
    // Check if the exception is caused by a data abort without a change in
    // exception level.
    cmp x1, 0x25
    b.ne .Lxcpt_sync_curr_el_sp_elx_handler_not_data_abort

    bl xcpt_data_abort_handler
    b .Lxcpt_sync_curr_el_sp_elx_handler_end

.Lxcpt_sync_curr_el_sp_elx_handler_not_data_abort:
    mov x0, 0x4
    bl xcpt_default_handler

.Lxcpt_sync_curr_el_sp_elx_handler_end:
    // We have to split the handler, since it uses more than 32 instructions.
    b load_aapcs_and_eret

.type xcpt_sync_curr_el_sp_elx_handler, function
.size xcpt_sync_curr_el_sp_elx_handler, . - xcpt_sync_curr_el_sp_elx_handler

.align 7

    // IRQ.
xcpt_irq_curr_el_sp_elx_handler:
    save_aapcs
    bl xcpt_irq_handler
    bl task_queue_sched
    load_aapcs
    eret

.type xcpt_irq_curr_el_sp_elx_handler, function
.size xcpt_irq_curr_el_sp_elx_handler, . - xcpt_irq_curr_el_sp_elx_handler

.align 7

    // FIQ.
    default_vt_entry 0x6 xcpt_fiq_curr_el_sp_elx_handler

    // SError.
    default_vt_entry 0x7 xcpt_serror_curr_el_sp_elx_handler

    // Exception from a lower EL and at least one lower EL is AArch64.

    // Synchronous.
xcpt_sync_lower_el_aarch64_handler:
    save_aapcs

    // Check if the exception is caused by svc.
    mrs x9, esr_el1
    ubfx x10, x9, 26, 6
    cmp x10, 0x15
    b.ne .Lxcpt_sync_lower_el_aarch64_handler_not_svc

    // The exception is caused by svc.
    bl xcpt_svc_handler
    b .Lxcpt_sync_lower_el_aarch64_handler_end

.Lxcpt_sync_lower_el_aarch64_handler_not_svc:
    // The exception is not caused by svc.

    // Check if the exception is caused by an instruction abort from a lower
    // exception level.
    cmp x10, 0x20
    b.ne .Lxcpt_sync_lower_el_aarch64_handler_not_insn_abort

    mov x0, x9
    bl xcpt_insn_abort_handler
    b .Lxcpt_sync_lower_el_aarch64_handler_end

.Lxcpt_sync_lower_el_aarch64_handler_not_insn_abort:
    // Check if the exception is caused by an data abort from a lower exception
    // level.
    cmp x10, 0x24
    b.ne .Lxcpt_sync_lower_el_aarch64_handler_not_data_abort

    mov x0, x9
    bl xcpt_data_abort_handler
    b .Lxcpt_sync_lower_el_aarch64_handler_end

.Lxcpt_sync_lower_el_aarch64_handler_not_data_abort:
    mov x0, 0x8
    bl xcpt_default_handler

.Lxcpt_sync_lower_el_aarch64_handler_end:
    bl handle_signals
    // We have to split the handler, since it uses more than 32 instructions.
    b load_aapcs_and_eret

.type xcpt_sync_lower_el_aarch64_handler, function
.size xcpt_sync_lower_el_aarch64_handler, . - xcpt_sync_lower_el_aarch64_handler

.align 7

    // IRQ.
xcpt_irq_lower_el_aarch64_handler:
    save_aapcs
    bl xcpt_irq_handler
    bl task_queue_sched
    bl handle_signals
    load_aapcs
    eret

.type xcpt_irq_lower_el_aarch64_handler, function
.size xcpt_irq_lower_el_aarch64_handler, . - xcpt_irq_lower_el_aarch64_handler

.align 7

    // FIQ.
    default_vt_entry 0xa xcpt_fiq_lower_el_aarch64_handler

    // SError.
    default_vt_entry 0xb xcpt_serror_lower_el_aarch64_handler

    // Exception from a lower EL and all lower ELs are AArch32.

    // Synchronous.
    default_vt_entry 0xc xcpt_sync_lower_el_aarch32_handler

    // IRQ.
    default_vt_entry 0xd xcpt_irq_lower_el_aarch32_handler

    // FIQ.
    default_vt_entry 0xe xcpt_fiq_lower_el_aarch32_handler

    // SError.
    default_vt_entry 0xf xcpt_serror_lower_el_aarch32_handler

.size xcpt_vector_table, . - xcpt_vector_table
.global xcpt_vector_table

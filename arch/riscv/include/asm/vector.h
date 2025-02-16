/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2020 SiFive
 */

#ifndef __ASM_RISCV_VECTOR_H
#define __ASM_RISCV_VECTOR_H

#include <linux/types.h>

#ifdef CONFIG_RISCV_ISA_V

#include <linux/stringify.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <asm/ptrace.h>
#include <asm/hwcap.h>
#include <asm/csr.h>
#include <asm/asm.h>

extern unsigned long riscv_v_vsize;
void riscv_v_setup_vsize(void);
bool riscv_v_first_use_handler(struct pt_regs *regs);
void kernel_vector_begin(void);
void kernel_vector_end(void);
void get_cpu_vector_context(void);
void put_cpu_vector_context(void);

static inline u32 riscv_v_flags(void)
{
	return current->thread.riscv_v_flags;
}

static __always_inline bool has_vector(void)
{
	//return riscv_has_extension_likely(RISCV_ISA_EXT_v);
	return true;
}

static inline void __riscv_v_vstate_clean(struct pt_regs *regs)
{
	regs->status = (regs->status & ~SR_VS) | SR_VS_CLEAN;
}

static inline void riscv_v_vstate_off(struct pt_regs *regs)
{
	regs->status = (regs->status & ~SR_VS) | SR_VS_OFF;
}

static inline void riscv_v_vstate_on(struct pt_regs *regs)
{
	regs->status = (regs->status & ~SR_VS) | SR_VS_INITIAL;
}

static inline bool riscv_v_vstate_query(struct pt_regs *regs)
{
	return (regs->status & SR_VS) != 0;
}

static __always_inline void riscv_v_enable(void)
{
	csr_set(CSR_SSTATUS, SR_VS);
}

static __always_inline void riscv_v_disable(void)
{
	csr_clear(CSR_SSTATUS, SR_VS);
}

static __always_inline void riscv_v_csr_init(void)
{
	riscv_v_enable();
	asm volatile (
		"csrw	" __stringify(CSR_VSTART) ", 0\n\t"
		"csrw	" __stringify(CSR_VCSR) ", 0\n\t"
		: : :);
	riscv_v_disable();
}

static __always_inline void __vstate_csr_save(struct __riscv_v_ext_state *dest)
{
	asm volatile (
		"csrr	%0, " __stringify(CSR_VSTART) "\n\t"
		"csrr	%1, " __stringify(CSR_VTYPE) "\n\t"
		"csrr	%2, " __stringify(CSR_VL) "\n\t"
		"csrr	%3, " __stringify(CSR_VCSR) "\n\t"
		: "=r" (dest->vstart), "=r" (dest->vtype), "=r" (dest->vl),
		  "=r" (dest->vcsr) : :);
}

static __always_inline void __vstate_csr_restore(struct __riscv_v_ext_state *src)
{
	asm volatile (
		".option push\n\t"
		".option arch, +v\n\t"
		"vsetvl	 x0, %2, %1\n\t"
		".option pop\n\t"
		"csrw	" __stringify(CSR_VSTART) ", %0\n\t"
		"csrw	" __stringify(CSR_VCSR) ", %3\n\t"
		: : "r" (src->vstart), "r" (src->vtype), "r" (src->vl),
		    "r" (src->vcsr) :);
}

static inline void __riscv_v_vstate_save(struct __riscv_v_ext_state *save_to,
					 void *datap)
{
	unsigned long vl;

	riscv_v_enable();
	__vstate_csr_save(save_to);
	asm volatile (
		".option push\n\t"
		".option arch, +v\n\t"
		"vsetvli	%0, x0, e8, m8, ta, ma\n\t"
		"vse8.v		v0, (%1)\n\t"
		"add		%1, %1, %0\n\t"
		"vse8.v		v8, (%1)\n\t"
		"add		%1, %1, %0\n\t"
		"vse8.v		v16, (%1)\n\t"
		"add		%1, %1, %0\n\t"
		"vse8.v		v24, (%1)\n\t"
		".option pop\n\t"
		: "=&r" (vl) : "r" (datap) : "memory");
	riscv_v_disable();
}

static inline void __riscv_v_vstate_restore(struct __riscv_v_ext_state *restore_from,
					    void *datap)
{
	unsigned long vl;

	riscv_v_enable();
	asm volatile (
		".option push\n\t"
		".option arch, +v\n\t"
		"vsetvli	%0, x0, e8, m8, ta, ma\n\t"
		"vle8.v		v0, (%1)\n\t"
		"add		%1, %1, %0\n\t"
		"vle8.v		v8, (%1)\n\t"
		"add		%1, %1, %0\n\t"
		"vle8.v		v16, (%1)\n\t"
		"add		%1, %1, %0\n\t"
		"vle8.v		v24, (%1)\n\t"
		".option pop\n\t"
		: "=&r" (vl) : "r" (datap) : "memory");
	__vstate_csr_restore(restore_from);
	riscv_v_disable();
}

static inline void riscv_v_vstate_save(struct task_struct *task,
				       struct pt_regs *regs)
{
	if ((regs->status & SR_VS) == SR_VS_DIRTY) {
		struct __riscv_v_ext_state *vstate = &task->thread.vstate;

		__riscv_v_vstate_save(vstate, vstate->datap);
		__riscv_v_vstate_clean(regs);
	}
}

static inline void riscv_v_vstate_restore(struct task_struct *task,
					  struct pt_regs *regs)
{
	if ((regs->status & SR_VS) != SR_VS_OFF) {
		struct __riscv_v_ext_state *vstate = &task->thread.vstate;

		__riscv_v_vstate_restore(vstate, vstate->datap);
		__riscv_v_vstate_clean(regs);
	}
}

static inline void __switch_to_vector(struct task_struct *prev,
				      struct task_struct *next)
{
	struct pt_regs *regs;

	regs = task_pt_regs(prev);
	riscv_v_vstate_save(prev, regs);
	riscv_v_vstate_restore(next, task_pt_regs(next));
}

#else /* ! CONFIG_RISCV_ISA_V  */

struct pt_regs;

static __always_inline bool has_vector(void) { return false; }
static inline bool riscv_v_first_use_handler(struct pt_regs *regs) { return false; }
static inline bool riscv_v_vstate_query(struct pt_regs *regs) { return false; }
#define riscv_v_vsize (0)
#define riscv_v_setup_vsize()			do {} while (0)
#define riscv_v_vstate_save(task, regs)		do {} while (0)
#define riscv_v_vstate_restore(task, regs)	do {} while (0)
#define __switch_to_vector(__prev, __next)	do {} while (0)
#define riscv_v_vstate_off(regs)		do {} while (0)
#define riscv_v_vstate_on(regs)			do {} while (0)
#define riscv_v_csr_init()				do {} while (0)

#endif /* CONFIG_RISCV_ISA_V */
/*
 * Return the implementation's vlen value.
 *
 * riscv_v_vsize contains the value of "32 vector registers with vlenb length"
 * so rebuild the vlen value in bits from it.
 */
static inline int riscv_vector_vlen(void)
{
	return riscv_v_vsize / 32 * 8;
}

#endif /* ! __ASM_RISCV_VECTOR_H */

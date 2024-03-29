/*
 *  arch/arm/include/asm/mmu_context.h
 *
 *  Copyright (C) 1996 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Changelog:
 *   27-06-1996	RMK	Created
 */
#ifndef __ASM_ARM_MMU_CONTEXT_H
#define __ASM_ARM_MMU_CONTEXT_H

#include <linux/compiler.h>
#include <linux/sched.h>
#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/proc-fns.h>
#include <asm-generic/mm_hooks.h>
#include <asm/fcse.h>

void __check_kvm_seq(struct mm_struct *mm);

#ifdef CONFIG_CPU_HAS_ASID

/*
 * On ARMv6, we have the following structure in the Context ID:
 *
 * 31                         7          0
 * +-------------------------+-----------+
 * |      process ID         |   ASID    |
 * +-------------------------+-----------+
 * |              context ID             |
 * +-------------------------------------+
 *
 * The ASID is used to tag entries in the CPU caches and TLBs.
 * The context ID is used by debuggers and trace logic, and
 * should be unique within all running processes.
 */
#define ASID_BITS		8
#define ASID_MASK		((~0) << ASID_BITS)
#define ASID_FIRST_VERSION	(1 << ASID_BITS)

extern unsigned int cpu_last_asid;
#ifdef CONFIG_SMP
DECLARE_PER_CPU(struct mm_struct *, current_mm);
#endif

void __init_new_context(struct task_struct *tsk, struct mm_struct *mm);
void __new_context(struct mm_struct *mm);

static inline void check_context(struct mm_struct *mm)
{
	/*
	 * This code is executed with interrupts enabled. Therefore,
	 * mm->context.id cannot be updated to the latest ASID version
	 * on a different CPU (and condition below not triggered)
	 * without first getting an IPI to reset the context. The
	 * alternative is to take a read_lock on mm->context.id_lock
	 * (after changing its type to rwlock_t).
	 */
	if (unlikely((mm->context.id ^ cpu_last_asid) >> ASID_BITS))
		__new_context(mm);

	if (unlikely(mm->context.kvm_seq != init_mm.context.kvm_seq))
		__check_kvm_seq(mm);
}

#define init_new_context(tsk,mm)	(__init_new_context(tsk,mm),0)

#else

static inline void check_context(struct mm_struct *mm)
{
#ifdef CONFIG_MMU
	if (unlikely(mm->context.kvm_seq != init_mm.context.kvm_seq))
		__check_kvm_seq(mm);
#endif
}

static inline int
init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
#ifdef CONFIG_ARM_FCSE
	int fcse_pid;

#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
	if (!mm->context.fcse.large) {
		fcse_pid = fcse_pid_alloc(mm);
		mm->context.fcse.pid = fcse_pid << FCSE_PID_SHIFT;
	} else {
		/* We are normally forking a process vith a virtual address
		   space larger than 32 MB, so its pid should be 0. */
		FCSE_BUG_ON(mm->context.fcse.pid);
		fcse_pid_reference(0);
	}
	/* If we are forking, set_pte_at will restore the correct high pages
	   count, and shared writable pages are write-protected again. */
	mm->context.fcse.shared_dirty_pages = 0;
	mm->context.fcse.high_pages = 0;
	mm->context.fcse.active = 0;
#else /* CONFIG_ARM_FCSE_GUARANTEED */
	fcse_pid = fcse_pid_alloc(mm);
	if (fcse_pid < 0) {
		/*
		 * Set mm pid to FCSE_PID_INVALID, as even when
		 * init_new_context fails, destroy_context is called.
		 */
		mm->context.fcse.pid = FCSE_PID_INVALID;
		return fcse_pid;
	}
	mm->context.fcse.pid = fcse_pid << FCSE_PID_SHIFT;
#endif /* CONFIG_ARM_FCSE_GUARANTEED */
	FCSE_BUG_ON(fcse_mm_in_cache(mm));
#endif /* CONFIG_ARM_FCSE */

	return 0;
}

#endif

static inline void destroy_context(struct mm_struct *mm)
{
#ifdef CONFIG_ARM_FCSE
#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
	FCSE_BUG_ON(mm->context.fcse.shared_dirty_pages);
	FCSE_BUG_ON(mm->context.fcse.high_pages);
#endif /* CONFIG_ARM_FCSE_BEST_EFFORT */
	if (mm->context.fcse.pid != FCSE_PID_INVALID)
		fcse_pid_free(mm);
#endif /* CONFIG_ARM_FCSE */
}

/*
 * This is called when "tsk" is about to enter lazy TLB mode.
 *
 * mm:  describes the currently active mm context
 * tsk: task which is entering lazy tlb
 * cpu: cpu number which is entering lazy tlb
 *
 * tsk->mm will be NULL
 */
static inline void
enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
}

/*
 * This is the actual mm switch as far as the scheduler
 * is concerned.  No registers are touched.  We avoid
 * calling the CPU specific function when the mm hasn't
 * actually changed.
 */
static inline void
__switch_mm(struct mm_struct *prev, struct mm_struct *next,
	  struct task_struct *tsk)
{
#ifdef CONFIG_MMU
	unsigned int cpu = ipipe_processor_id();

#ifdef CONFIG_SMP
	/* check for possible thread migration */
	if (!cpumask_empty(mm_cpumask(next)) &&
	    !cpumask_test_cpu(cpu, mm_cpumask(next)))
		__flush_icache_all();
#endif
	if (!cpumask_test_and_set_cpu(cpu, mm_cpumask(next)) || prev != next) {
#ifdef CONFIG_SMP
		struct mm_struct **crt_mm = &per_cpu(current_mm, cpu);
#endif
#if defined(CONFIG_IPIPE)
		if (ipipe_root_domain_p) {
			/* mark mm state as undefined. */
			per_cpu(ipipe_active_mm, cpu) = NULL;
			barrier();
#ifdef CONFIG_SMP
			*crt_mm = next;
#endif
			check_context(next);
			cpu_switch_mm(next->pgd, next,
				      fcse_switch_mm(prev, next));
			barrier();
			per_cpu(ipipe_active_mm, cpu) = next;
			while (test_and_clear_thread_flag(TIF_MMSWITCH_INT)) {
				/* mark mm state as undefined. */
				per_cpu(ipipe_active_mm, cpu) = NULL;
#ifdef CONFIG_SMP
				*crt_mm = next;
#endif
				barrier();
				check_context(next);
				cpu_switch_mm(next->pgd, next,
					      fcse_switch_mm(NULL, next));
				barrier();
				per_cpu(ipipe_active_mm, cpu) = next;
			}
		} else
#endif /* CONFIG_IPIPE */
		{
#ifdef CONFIG_SMP
			*crt_mm = next;
#endif
			check_context(next);
			cpu_switch_mm(next->pgd, next,
				      fcse_switch_mm(prev, next));
		}
#if defined(CONFIG_IPIPE) && defined(CONFIG_ARM_FCSE)
		if (tsk)
			set_tsk_thread_flag(tsk, TIF_SWITCHED);
#endif /* CONFIG_IPIPE && CONFIG_ARM_FCSE */
		if (cache_is_vivt() && prev)
			cpumask_clear_cpu(cpu, mm_cpumask(prev));
	} else
		fcse_mark_dirty(next);
#endif
}

static inline void
switch_mm(struct mm_struct *prev, struct mm_struct *next,
	  struct task_struct *tsk)
{
#if !defined(CONFIG_IPIPE_WANT_PREEMPTIBLE_SWITCH) && !defined(CONFIG_SMP)
	unsigned long flags;
	local_irq_save_hw(flags);
#endif /* !(CONFIG_IPIPE_WANT_PREEMPTIBLE_SWITCH && SMP) */
	__switch_mm(prev, next, tsk);
#if !defined(CONFIG_IPIPE_WANT_PREEMPTIBLE_SWITCH) && !defined(CONFIG_SMP)
	local_irq_restore_hw(flags);
#endif /* !(CONFIG_IPIPE_WANT_PREEMPTIBLE_SWITCH && SMP) */
}

#define deactivate_mm(tsk,mm)	do { } while (0)

#ifndef CONFIG_ARM_FCSE_BEST_EFFORT
#define activate_mm(prev,next)	switch_mm(prev, next, NULL)
#else
#define activate_mm(prev,next)						\
	({								\
	switch_mm(prev, next, NULL);					\
	next->context.fcse.active = 1;					\
	FCSE_BUG_ON(current->mm == next && !fcse_mm_in_cache(next));	\
	})
#endif

#endif

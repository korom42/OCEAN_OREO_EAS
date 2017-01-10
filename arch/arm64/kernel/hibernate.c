/*:
 * Hibernate support specific for ARM64
 *
 * Derived from work on ARM hibernation support by:
 *
 * Ubuntu project, hibernation support for mach-dove
 * Copyright (C) 2010 Nokia Corporation (Hiroshi Doyu)
 * Copyright (C) 2010 Texas Instruments, Inc. (Teerth Reddy et al.)
 *  https://lkml.org/lkml/2010/6/18/4
 *  https://lists.linux-foundation.org/pipermail/linux-pm/2010-June/027422.html
 *  https://patchwork.kernel.org/patch/96442/
 *
 * Copyright (C) 2006 Rafael J. Wysocki <rjw@sisk.pl>
 *
 * License terms: GNU General Public License (GPL) version 2
 */
#define pr_fmt(x) "hibernate: " x
#include <linux/kvm_host.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/pm.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <linux/utsname.h>
#include <linux/version.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/irqflags.h>
#include <asm/memory.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/pgtable-hwdef.h>
#include <asm/sections.h>
#include <asm/suspend.h>
#include <asm/virt.h>

/*
 * Hibernate core relies on this value being 0 on resume, and marks it
 * __nosavedata assuming it will keep the resume kernel's '0' value. This
 * doesn't happen with either KASLR.
 *
 * defined as "__visible int in_suspend __nosavedata" in
 * kernel/power/hibernate.c
 */
extern int in_suspend;

/* Do we need to reset el2? */
#define el2_reset_needed() (is_hyp_mode_available() && !is_kernel_in_hyp_mode())

/*
 * Start/end of the hibernate exit code, this must be copied to a 'safe'
 * location in memory, and executed from there.
 */
extern char __hibernate_exit_text_start[], __hibernate_exit_text_end[];

/* temporary el2 vectors in the __hibernate_exit_text section. */
extern char hibernate_el2_vectors[];

/* hyp-stub vectors, used to restore el2 during resume from hibernate. */
extern char __hyp_stub_vectors[];

/*
 * Values that may not change over hibernate/resume. We put the build number
 * and date in here so that we guarantee not to resume with a different
 * kernel.
 */
struct arch_hibernate_hdr_invariants {
	char		uts_version[__NEW_UTS_LEN + 1];
};

/* These values need to be know across a hibernate/restore. */
static struct arch_hibernate_hdr {
	struct arch_hibernate_hdr_invariants invariants;

	/* These are needed to find the relocated kernel if built with kaslr */
	phys_addr_t	ttbr1_el1;
	void		(*reenter_kernel)(void);

	/*
	 * We need to know where the __hyp_stub_vectors are after restore to
	 * re-configure el2.
	 */
	phys_addr_t	__hyp_stub_vectors;
} resume_hdr;

static inline void arch_hdr_invariants(struct arch_hibernate_hdr_invariants *i)
{
	memset(i, 0, sizeof(*i));
	memcpy(i->uts_version, init_utsname()->version, sizeof(i->uts_version));
}

int pfn_is_nosave(unsigned long pfn)
{
	unsigned long nosave_begin_pfn = sym_to_pfn(&__nosave_begin);
	unsigned long nosave_end_pfn = sym_to_pfn(&__nosave_end - 1);

	return (pfn >= nosave_begin_pfn) && (pfn <= nosave_end_pfn);
}

void notrace save_processor_state(void)
{
	WARN_ON(num_online_cpus() != 1);
}

void notrace restore_processor_state(void)
{
}

int arch_hibernation_header_save(void *addr, unsigned int max_size)
{
	struct arch_hibernate_hdr *hdr = addr;

	if (max_size < sizeof(*hdr))
		return -EOVERFLOW;

	arch_hdr_invariants(&hdr->invariants);
	hdr->ttbr1_el1		= __pa_symbol(swapper_pg_dir);
	hdr->reenter_kernel	= _cpu_resume;

	/* We can't use __hyp_get_vectors() because kvm may still be loaded */
	if (el2_reset_needed())
		hdr->__hyp_stub_vectors = __pa_symbol(__hyp_stub_vectors);
	else
		hdr->__hyp_stub_vectors = 0;

	return 0;
}
EXPORT_SYMBOL(arch_hibernation_header_save);

int arch_hibernation_header_restore(void *addr)
{
	struct arch_hibernate_hdr_invariants invariants;
	struct arch_hibernate_hdr *hdr = addr;

	arch_hdr_invariants(&invariants);
	if (memcmp(&hdr->invariants, &invariants, sizeof(invariants))) {
		pr_crit("Hibernate image not generated by this kernel!\n");
		return -EINVAL;
	}

	resume_hdr = *hdr;

	return 0;
}
EXPORT_SYMBOL(arch_hibernation_header_restore);

/*
 * Copies length bytes, starting at src_start into an new page,
 * perform cache maintentance, then maps it at the specified address low
 * address as executable.
 *
 * This is used by hibernate to copy the code it needs to execute when
 * overwriting the kernel text. This function generates a new set of page
 * tables, which it loads into ttbr0.
 *
 * Length is provided as we probably only want 4K of data, even on a 64K
 * page system.
 */
static int create_safe_exec_page(void *src_start, size_t length,
				 unsigned long dst_addr,
				 phys_addr_t *phys_dst_addr,
				 void *(*allocator)(gfp_t mask),
				 gfp_t mask)
{
	int rc = 0;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long dst = (unsigned long)allocator(mask);

	if (!dst) {
		rc = -ENOMEM;
		goto out;
	}

	memcpy((void *)dst, src_start, length);
	flush_icache_range(dst, dst + length);

	pgd = pgd_offset_raw(allocator(mask), dst_addr);
	if (pgd_none(*pgd)) {
		pud = allocator(mask);
		if (!pud) {
			rc = -ENOMEM;
			goto out;
		}
		pgd_populate(&init_mm, pgd, pud);
	}

	pud = pud_offset(pgd, dst_addr);
	if (pud_none(*pud)) {
		pmd = allocator(mask);
		if (!pmd) {
			rc = -ENOMEM;
			goto out;
		}
		pud_populate(&init_mm, pud, pmd);
	}

	pmd = pmd_offset(pud, dst_addr);
	if (pmd_none(*pmd)) {
		pte = allocator(mask);
		if (!pte) {
			rc = -ENOMEM;
			goto out;
		}
		pmd_populate_kernel(&init_mm, pmd, pte);
	}

	pte = pte_offset_kernel(pmd, dst_addr);
	set_pte(pte, __pte(virt_to_phys((void *)dst) |
			 pgprot_val(PAGE_KERNEL_EXEC)));

	/* Load our new page tables */
	asm volatile("msr	ttbr0_el1, %0;"
		     "isb;"
		     "tlbi	vmalle1is;"
		     "dsb	ish;"
		     "isb" : : "r"(virt_to_phys(pgd)));

	*phys_dst_addr = virt_to_phys((void *)dst);

out:
	return rc;
}


int swsusp_arch_suspend(void)
{
	int ret = 0;
	unsigned long flags;
	struct sleep_stack_data state;

	local_dbg_save(flags);

	if (__cpu_suspend_enter(&state)) {
		ret = swsusp_save();
	} else {
		/* Clean kernel to PoC for secondary core startup */
		__flush_dcache_area(LMADDR(KERNEL_START), KERNEL_END - KERNEL_START);

		/*
		 * Tell the hibernation core that we've just restored
		 * the memory
		 */
		in_suspend = 0;

		__cpu_suspend_exit();
	}

	local_dbg_restore(flags);

	return ret;
}

static int copy_pte(pmd_t *dst_pmd, pmd_t *src_pmd, unsigned long start,
		    unsigned long end)
{
	pte_t *src_pte;
	pte_t *dst_pte;
	unsigned long addr = start;

	dst_pte = (pte_t *)get_safe_page(GFP_ATOMIC);
	if (!dst_pte)
		return -ENOMEM;
	pmd_populate_kernel(&init_mm, dst_pmd, dst_pte);
	dst_pte = pte_offset_kernel(dst_pmd, start);

	src_pte = pte_offset_kernel(src_pmd, start);
	do {
		if (!pte_none(*src_pte))
			/*
			 * Resume will overwrite areas that may be marked
			 * read only (code, rodata). Clear the RDONLY bit from
			 * the temporary mappings we use during restore.
			 */
			set_pte(dst_pte, __pte(pte_val(*src_pte) & ~PTE_RDONLY));
	} while (dst_pte++, src_pte++, addr += PAGE_SIZE, addr != end);

	return 0;
}

static int copy_pmd(pud_t *dst_pud, pud_t *src_pud, unsigned long start,
		    unsigned long end)
{
	pmd_t *src_pmd;
	pmd_t *dst_pmd;
	unsigned long next;
	unsigned long addr = start;

	if (pud_none(*dst_pud)) {
		dst_pmd = (pmd_t *)get_safe_page(GFP_ATOMIC);
		if (!dst_pmd)
			return -ENOMEM;
		pud_populate(&init_mm, dst_pud, dst_pmd);
	}
	dst_pmd = pmd_offset(dst_pud, start);

	src_pmd = pmd_offset(src_pud, start);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*src_pmd))
			continue;
		if (pmd_table(*src_pmd)) {
			if (copy_pte(dst_pmd, src_pmd, addr, next))
				return -ENOMEM;
		} else {
			set_pmd(dst_pmd,
				__pmd(pmd_val(*src_pmd) & ~PMD_SECT_RDONLY));
		}
	} while (dst_pmd++, src_pmd++, addr = next, addr != end);

	return 0;
}

static int copy_pud(pgd_t *dst_pgd, pgd_t *src_pgd, unsigned long start,
		    unsigned long end)
{
	pud_t *dst_pud;
	pud_t *src_pud;
	unsigned long next;
	unsigned long addr = start;

	if (pgd_none(*dst_pgd)) {
		dst_pud = (pud_t *)get_safe_page(GFP_ATOMIC);
		if (!dst_pud)
			return -ENOMEM;
		pgd_populate(&init_mm, dst_pgd, dst_pud);
	}
	dst_pud = pud_offset(dst_pgd, start);

	src_pud = pud_offset(src_pgd, start);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none(*src_pud))
			continue;
		if (pud_table(*(src_pud))) {
			if (copy_pmd(dst_pud, src_pud, addr, next))
				return -ENOMEM;
		} else {
			set_pud(dst_pud,
				__pud(pud_val(*src_pud) & ~PMD_SECT_RDONLY));
		}
	} while (dst_pud++, src_pud++, addr = next, addr != end);

	return 0;
}

static int copy_page_tables(pgd_t *dst_pgd, unsigned long start,
			    unsigned long end)
{
	unsigned long next;
	unsigned long addr = start;
	pgd_t *src_pgd = pgd_offset_k(start);

	dst_pgd = pgd_offset_raw(dst_pgd, start);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*src_pgd))
			continue;
		if (copy_pud(dst_pgd, src_pgd, addr, next))
			return -ENOMEM;
	} while (dst_pgd++, src_pgd++, addr = next, addr != end);

	return 0;
}

/*
 * Setup then Resume from the hibernate image using swsusp_arch_suspend_exit().
 *
 * Memory allocated by get_safe_page() will be dealt with by the hibernate code,
 * we don't need to free it here.
 */
int swsusp_arch_resume(void)
{
	int rc = 0;
	void *zero_page;
	size_t exit_size;
	pgd_t *tmp_pg_dir;
	phys_addr_t phys_hibernate_exit;
	void __noreturn (*hibernate_exit)(phys_addr_t, phys_addr_t, void *,
					  void *, phys_addr_t, phys_addr_t);

	/*
	 * Locate the exit code in the bottom-but-one page, so that *NULL
	 * still has disastrous affects.
	 */
	hibernate_exit = (void *)PAGE_SIZE;
	exit_size = __hibernate_exit_text_end - __hibernate_exit_text_start;
	/*
	 * Copy swsusp_arch_suspend_exit() to a safe page. This will generate
	 * a new set of ttbr0 page tables and load them.
	 */
	rc = create_safe_exec_page(__hibernate_exit_text_start, exit_size,
				   (unsigned long)hibernate_exit,
				   &phys_hibernate_exit,
				   (void *)get_safe_page, GFP_ATOMIC);
	if (rc) {
		pr_err("Failed to create safe executable page for hibernate_exit code.");
		goto out;
	}

	/*
	 * The hibernate exit text contains a set of el2 vectors, that will
	 * be executed at el2 with the mmu off in order to reload hyp-stub.
	 */
	__flush_dcache_area(hibernate_exit, exit_size);

	/*
	 * Restoring the memory image will overwrite the ttbr1 page tables.
	 * Create a second copy of just the linear map, and use this when
	 * restoring.
	 */
	tmp_pg_dir = (pgd_t *)get_safe_page(GFP_ATOMIC);
	if (!tmp_pg_dir) {
		pr_err("Failed to allocate memory for temporary page tables.");
		rc = -ENOMEM;
		goto out;
	}
	rc = copy_page_tables(tmp_pg_dir, PAGE_OFFSET, 0);
	if (rc)
		goto out;

	/*
	 * Since we only copied the linear map, we need to find restore_pblist's
	 * linear map address.
	 */
	lm_restore_pblist = LMADDR(restore_pblist);

	/*
	 * KASLR will cause the el2 vectors to be in a different location in
	 * the resumed kernel. Load hibernate's temporary copy into el2.
	 *
	 * We can skip this step if we booted at EL1, or are running with VHE.
	 */
	if (el2_reset_needed()) {
		phys_addr_t el2_vectors = phys_hibernate_exit;  /* base */
		el2_vectors += hibernate_el2_vectors -
			       __hibernate_exit_text_start;     /* offset */

		__hyp_set_vectors(el2_vectors);
	}

	/*
	 * We need a zero page that is zero before & after resume in order to
	 * to break before make on the ttbr1 page tables.
	 */
	zero_page = (void *)get_safe_page(GFP_ATOMIC);

	hibernate_exit(virt_to_phys(tmp_pg_dir), resume_hdr.ttbr1_el1,
		       resume_hdr.reenter_kernel, restore_pblist,
		       resume_hdr.__hyp_stub_vectors, virt_to_phys(zero_page));

out:
	return rc;
}

static int check_boot_cpu_online_pm_callback(struct notifier_block *nb,
					     unsigned long action, void *ptr)
{
	if (action == PM_HIBERNATION_PREPARE &&
	     cpumask_first(cpu_online_mask) != 0) {
		pr_warn("CPU0 is offline.\n");
		return notifier_from_errno(-ENODEV);
	}

	return NOTIFY_OK;
}

static int __init check_boot_cpu_online_init(void)
{
	/*
	 * Set this pm_notifier callback with a lower priority than
	 * cpu_hotplug_pm_callback, so that cpu_hotplug_pm_callback will be
	 * called earlier to disable cpu hotplug before the cpu online check.
	 */
	pm_notifier(check_boot_cpu_online_pm_callback, -INT_MAX);

	return 0;
}
core_initcall(check_boot_cpu_online_init);

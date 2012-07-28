/* Copyright (c) 2002,2007-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/genalloc.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/bitmap.h>
#ifdef CONFIG_MSM_KGSL_MMU
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#endif
#include "kgsl_mmu.h"
#include "kgsl_drawctxt.h"
#include "kgsl.h"
#include "kgsl_log.h"
#include "kgsl_device.h"

struct kgsl_pte_debug {
	unsigned int read:1;
	unsigned int write:1;
	unsigned int dirty:1;
	unsigned int reserved:9;
	unsigned int phyaddr:20;
};

#define GSL_PT_PAGE_BITS_MASK	0x00000007
#define GSL_PT_PAGE_ADDR_MASK	PAGE_MASK

/// HTC:
#define MMU_MAP_WARNING_ALLOC_SIZE	(16 * 1024 * 1024)
/// :HTC

#define GSL_MMU_INT_MASK \
	(MH_INTERRUPT_MASK__AXI_READ_ERROR | \
	 MH_INTERRUPT_MASK__AXI_WRITE_ERROR)

/* pt_mutex needs to be held in this function */

static struct kgsl_pagetable *
kgsl_get_pagetable(unsigned long name)
{
	struct kgsl_pagetable *pt;

	list_for_each_entry(pt,	&kgsl_driver.pagetable_list, list) {
		if (pt->name == name)
			return pt;
	}

	return NULL;
}

static struct kgsl_pagetable *
_get_pt_from_kobj(struct kobject *kobj)
{
	unsigned long ptname;

	if (!kobj)
		return NULL;

	if (sscanf(kobj->name, "%ld", &ptname) != 1)
		return NULL;

	return kgsl_get_pagetable(ptname);
}

static ssize_t
sysfs_show_entries(struct kobject *kobj,
		   struct kobj_attribute *attr,
		   char *buf)
{
	struct kgsl_pagetable *pt;
	int ret = 0;

	mutex_lock(&kgsl_driver.pt_mutex);
	pt = _get_pt_from_kobj(kobj);

	if (pt)
		ret += sprintf(buf, "%d\n", pt->stats.entries);

	mutex_unlock(&kgsl_driver.pt_mutex);
	return ret;
}

static ssize_t
sysfs_show_mapped(struct kobject *kobj,
		  struct kobj_attribute *attr,
		  char *buf)
{
	struct kgsl_pagetable *pt;
	int ret = 0;

	mutex_lock(&kgsl_driver.pt_mutex);
	pt = _get_pt_from_kobj(kobj);

	if (pt)
		ret += sprintf(buf, "%d\n", pt->stats.mapped);

	mutex_unlock(&kgsl_driver.pt_mutex);
	return ret;
}

static ssize_t
sysfs_show_va_range(struct kobject *kobj,
		    struct kobj_attribute *attr,
		    char *buf)
{
	struct kgsl_pagetable *pt;
	int ret = 0;

	mutex_lock(&kgsl_driver.pt_mutex);
	pt = _get_pt_from_kobj(kobj);

	if (pt)
		ret += sprintf(buf, "0x%x\n", pt->va_range);

	mutex_unlock(&kgsl_driver.pt_mutex);
	return ret;
}

static ssize_t
sysfs_show_max_mapped(struct kobject *kobj,
		      struct kobj_attribute *attr,
		      char *buf)
{
	struct kgsl_pagetable *pt;
	int ret = 0;

	mutex_lock(&kgsl_driver.pt_mutex);
	pt = _get_pt_from_kobj(kobj);

	if (pt)
		ret += sprintf(buf, "%d\n", pt->stats.max_mapped);

	mutex_unlock(&kgsl_driver.pt_mutex);
	return ret;
}

static ssize_t
sysfs_show_max_entries(struct kobject *kobj,
		       struct kobj_attribute *attr,
		       char *buf)
{
	struct kgsl_pagetable *pt;
	int ret = 0;

	mutex_lock(&kgsl_driver.pt_mutex);
	pt = _get_pt_from_kobj(kobj);

	if (pt)
		ret += sprintf(buf, "%d\n", pt->stats.max_entries);

	mutex_unlock(&kgsl_driver.pt_mutex);
	return ret;
}

static struct kobj_attribute attr_entries = {
	.attr = { .name = "entries", .mode = 0444 },
	.show = sysfs_show_entries,
	.store = NULL,
};

static struct kobj_attribute attr_mapped = {
	.attr = { .name = "mapped", .mode = 0444 },
	.show = sysfs_show_mapped,
	.store = NULL,
};

static struct kobj_attribute attr_va_range = {
	.attr = { .name = "va_range", .mode = 0444 },
	.show = sysfs_show_va_range,
	.store = NULL,
};

static struct kobj_attribute attr_max_mapped = {
	.attr = { .name = "max_mapped", .mode = 0444 },
	.show = sysfs_show_max_mapped,
	.store = NULL,
};

static struct kobj_attribute attr_max_entries = {
	.attr = { .name = "max_entries", .mode = 0444 },
	.show = sysfs_show_max_entries,
	.store = NULL,
};

static struct attribute *pagetable_attrs[] = {
	&attr_entries.attr,
	&attr_mapped.attr,
	&attr_va_range.attr,
	&attr_max_mapped.attr,
	&attr_max_entries.attr,
	NULL,
};

static struct attribute_group pagetable_attr_group = {
	.attrs = pagetable_attrs,
};

static void
pagetable_remove_sysfs_objects(struct kgsl_pagetable *pagetable)
{
	if (pagetable->kobj)
		sysfs_remove_group(pagetable->kobj,
				   &pagetable_attr_group);

	kobject_put(pagetable->kobj);
}

static int
pagetable_add_sysfs_objects(struct kgsl_pagetable *pagetable)
{
	char ptname[16];
	int ret = -ENOMEM;

	snprintf(ptname, sizeof(ptname), "%d", pagetable->name);
	pagetable->kobj = kobject_create_and_add(ptname,
						 kgsl_driver.ptkobj);
	if (pagetable->kobj == NULL)
		goto err;

	ret = sysfs_create_group(pagetable->kobj, &pagetable_attr_group);

err:
	if (ret) {
		if (pagetable->kobj)
			kobject_put(pagetable->kobj);

		pagetable->kobj = NULL;
	}

	return ret;
}

static inline uint32_t
kgsl_pt_entry_get(struct kgsl_pagetable *pt, uint32_t va)
{
	return (va - pt->va_base) >> PAGE_SHIFT;
}

static inline void
kgsl_pt_map_set(struct kgsl_pagetable *pt, uint32_t pte, uint32_t val)
{
	uint32_t *baseptr = (uint32_t *)pt->base.hostptr;
	writel(val, &baseptr[pte]);
}

static inline uint32_t
kgsl_pt_map_getaddr(struct kgsl_pagetable *pt, uint32_t pte)
{
	uint32_t *baseptr = (uint32_t *)pt->base.hostptr;
	return readl(&baseptr[pte]) & GSL_PT_PAGE_ADDR_MASK;
}

void kgsl_mh_intrcallback(struct kgsl_device *device)
{
	unsigned int status = 0;
	unsigned int reg;

	kgsl_regread_isr(device, device->mmu.reg.interrupt_status, &status);

	if (status & MH_INTERRUPT_MASK__AXI_READ_ERROR) {
		kgsl_regread_isr(device, device->mmu.reg.axi_error, &reg);
		KGSL_MEM_CRIT(device, "axi read error interrupt: %08x\n", reg);
	} else if (status & MH_INTERRUPT_MASK__AXI_WRITE_ERROR) {
		kgsl_regread_isr(device, device->mmu.reg.axi_error, &reg);
		KGSL_MEM_CRIT(device, "axi write error interrupt: %08x\n", reg);
	} else if (status & MH_INTERRUPT_MASK__MMU_PAGE_FAULT) {
		kgsl_regread_isr(device, device->mmu.reg.page_fault, &reg);
		KGSL_MEM_CRIT(device, "mmu page fault interrupt: %08x\n", reg);
	} else {
		KGSL_MEM_WARN(device,
			"bad bits in REG_MH_INTERRUPT_STATUS %08x\n", status);
	}

	kgsl_regwrite_isr(device, device->mmu.reg.interrupt_clear, status);

	/*TODO: figure out how to handle errror interupts.
	* specifically, page faults should probably nuke the client that
	* caused them, but we don't have enough info to figure that out yet.
	*/
}

static int
kgsl_ptpool_get(struct kgsl_memdesc *memdesc)
{
	int pt;
	unsigned long flags;

	spin_lock_irqsave(&kgsl_driver.ptpool.lock, flags);

	pt = find_next_zero_bit(kgsl_driver.ptpool.bitmap,
				kgsl_pagetable_count, 0);

	if (pt >= kgsl_pagetable_count) {
		spin_unlock_irqrestore(&kgsl_driver.ptpool.lock, flags);
		return -ENOMEM;
	}

	set_bit(pt, kgsl_driver.ptpool.bitmap);

	spin_unlock_irqrestore(&kgsl_driver.ptpool.lock, flags);

	/* The memory is zeroed at init time and when page tables are
	   freed.0 This saves us from having to do the memset here */

	memdesc->hostptr = kgsl_driver.ptpool.hostptr +
		(pt * KGSL_PAGETABLE_SIZE);

	memdesc->physaddr = kgsl_driver.ptpool.physaddr +
		(pt * KGSL_PAGETABLE_SIZE);

	memdesc->size = KGSL_PAGETABLE_SIZE;

	return 0;
}

static void
kgsl_ptpool_put(struct kgsl_memdesc *memdesc)
{
	int pt;
	unsigned long flags;

	if (memdesc->hostptr == NULL)
		return;

	pt = (memdesc->hostptr - kgsl_driver.ptpool.hostptr)
		/ KGSL_PAGETABLE_SIZE;

	/* Clear the memory now to avoid having to do it next time
	   these entries are allocated */

	memset(memdesc->hostptr, 0, memdesc->size);

	spin_lock_irqsave(&kgsl_driver.ptpool.lock, flags);
	clear_bit(pt, kgsl_driver.ptpool.bitmap);
	spin_unlock_irqrestore(&kgsl_driver.ptpool.lock, flags);
}

static struct kgsl_pagetable *kgsl_mmu_createpagetableobject(
				unsigned int name)
{
	int status = 0;
	struct kgsl_pagetable *pagetable = NULL;

	pagetable = kzalloc(sizeof(struct kgsl_pagetable), GFP_KERNEL);
	if (pagetable == NULL) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n",
			sizeof(struct kgsl_pagetable));
		return NULL;
	}

	pagetable->refcnt = 1;

	spin_lock_init(&pagetable->lock);
	pagetable->tlb_flags = 0;
	pagetable->name = name;
	pagetable->va_base = KGSL_PAGETABLE_BASE;
	pagetable->va_range = CONFIG_MSM_KGSL_PAGE_TABLE_SIZE;
	pagetable->last_superpte = 0;
	pagetable->max_entries = KGSL_PAGETABLE_ENTRIES(pagetable->va_range);

	pagetable->tlbflushfilter.size = (pagetable->va_range /
				(PAGE_SIZE * GSL_PT_SUPER_PTE * 8)) + 1;
	pagetable->tlbflushfilter.base = (unsigned int *)
			kzalloc(pagetable->tlbflushfilter.size, GFP_KERNEL);
	if (!pagetable->tlbflushfilter.base) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n",
			pagetable->tlbflushfilter.size);
		goto err_alloc;
	}
	GSL_TLBFLUSH_FILTER_RESET();

	pagetable->pool = gen_pool_create(PAGE_SHIFT, -1);
	if (pagetable->pool == NULL) {
		KGSL_CORE_ERR("gen_pool_create(%d) failed\n", PAGE_SHIFT);
		goto err_flushfilter;
	}

	if (gen_pool_add(pagetable->pool, pagetable->va_base,
				pagetable->va_range, -1)) {
		KGSL_CORE_ERR("gen_pool_add failed\n");
		goto err_pool;
	}

	/* allocate page table memory */
	status = kgsl_ptpool_get(&pagetable->base);

	if (status != 0)
		goto err_pool;

	/* ptpool allocations are from coherent memory, so update the
	   device statistics acordingly */

	KGSL_STATS_ADD(KGSL_PAGETABLE_SIZE, kgsl_driver.stats.coherent,
		       kgsl_driver.stats.coherent_max);

	pagetable->base.gpuaddr = pagetable->base.physaddr;
	pagetable->base.size = KGSL_PAGETABLE_SIZE;

	status = kgsl_setup_pt(pagetable);
	if (status)
		goto err_free_sharedmem;

	list_add(&pagetable->list, &kgsl_driver.pagetable_list);

	/* Create the sysfs entries */
	pagetable_add_sysfs_objects(pagetable);

	return pagetable;

err_free_sharedmem:
	kgsl_ptpool_put(&pagetable->base);
err_pool:
	gen_pool_destroy(pagetable->pool);
err_flushfilter:
	kfree(pagetable->tlbflushfilter.base);
err_alloc:
	kfree(pagetable);

	return NULL;
}

static void kgsl_mmu_destroypagetable(struct kgsl_pagetable *pagetable)
{
	list_del(&pagetable->list);

	pagetable_remove_sysfs_objects(pagetable);

	kgsl_cleanup_pt(pagetable);

	kgsl_ptpool_put(&pagetable->base);

	kgsl_driver.stats.coherent -= KGSL_PAGETABLE_SIZE;

	if (pagetable->pool) {
		gen_pool_destroy(pagetable->pool);
		pagetable->pool = NULL;
	}

	if (pagetable->tlbflushfilter.base) {
		pagetable->tlbflushfilter.size = 0;
		kfree(pagetable->tlbflushfilter.base);
		pagetable->tlbflushfilter.base = NULL;
	}

	kfree(pagetable);
}

struct kgsl_pagetable *kgsl_mmu_getpagetable(unsigned long name)
{
	struct kgsl_pagetable *pt;

	mutex_lock(&kgsl_driver.pt_mutex);

	pt = kgsl_get_pagetable(name);

	if (pt) {
		spin_lock(&pt->lock);
		pt->refcnt++;
		spin_unlock(&pt->lock);
		goto done;
	}

	pt = kgsl_mmu_createpagetableobject(name);

done:
	mutex_unlock(&kgsl_driver.pt_mutex);
	return pt;
}

void kgsl_mmu_putpagetable(struct kgsl_pagetable *pagetable)
{
	bool dead;
	if (pagetable == NULL)
		return;

	mutex_lock(&kgsl_driver.pt_mutex);

	spin_lock(&pagetable->lock);
	dead = (--pagetable->refcnt) == 0;
	spin_unlock(&pagetable->lock);

	if (dead)
		kgsl_mmu_destroypagetable(pagetable);

	mutex_unlock(&kgsl_driver.pt_mutex);
}

int kgsl_mmu_setstate(struct kgsl_device *device,
				struct kgsl_pagetable *pagetable)
{
	int status = 0;
	struct kgsl_mmu *mmu = &device->mmu;

	if (mmu->flags & KGSL_FLAGS_STARTED) {
		/* page table not current, then setup mmu to use new
		 *  specified page table
		 */
		if (mmu->hwpagetable != pagetable) {
			mmu->hwpagetable = pagetable;
			spin_lock(&mmu->hwpagetable->lock);
			mmu->hwpagetable->tlb_flags &= ~(1<<device->id);
			spin_unlock(&mmu->hwpagetable->lock);

			/* call device specific set page table */
			status = kgsl_setstate(mmu->device,
				KGSL_MMUFLAGS_TLBFLUSH |
				KGSL_MMUFLAGS_PTUPDATE);

		}
	}

	return status;
}

int kgsl_mmu_init(struct kgsl_device *device)
{
	/*
	 * intialize device mmu
	 *
	 * call this with the global lock held
	 */
	int status = 0;
	struct kgsl_mmu *mmu = &device->mmu;

	mmu->device = device;

#ifndef CONFIG_MSM_KGSL_MMU
	mmu->config = 0x00000000;
#endif

	/* MMU not enabled */
	if ((mmu->config & 0x1) == 0)
		return 0;

	/* make sure aligned to pagesize */
	BUG_ON(mmu->mpu_base & (PAGE_SIZE - 1));
	BUG_ON((mmu->mpu_base + mmu->mpu_range) & (PAGE_SIZE - 1));

	/* sub-client MMU lookups require address translation */
	if ((mmu->config & ~0x1) > 0) {
		/*make sure virtual address range is a multiple of 64Kb */
		BUG_ON(CONFIG_MSM_KGSL_PAGE_TABLE_SIZE & ((1 << 16) - 1));

		/* allocate memory used for completing r/w operations that
		 * cannot be mapped by the MMU
		 */
		status = kgsl_sharedmem_alloc_coherent(&mmu->dummyspace, 64);
		if (!status)
			kgsl_sharedmem_set(&mmu->dummyspace, 0, 0,
					   mmu->dummyspace.size);
	}

	return status;
}

int kgsl_mmu_start(struct kgsl_device *device)
{
	/*
	 * intialize device mmu
	 *
	 * call this with the global lock held
	 */
	int status;
	struct kgsl_mmu *mmu = &device->mmu;

	if (mmu->flags & KGSL_FLAGS_STARTED)
		return 0;

	/* MMU not enabled */
	if ((mmu->config & 0x1) == 0)
		return 0;

	mmu->flags |= KGSL_FLAGS_STARTED;

	/* setup MMU and sub-client behavior */
	kgsl_regwrite(device, device->mmu.reg.config, mmu->config);

	/* enable axi interrupts */
	kgsl_regwrite(device, device->mmu.reg.interrupt_mask,
				GSL_MMU_INT_MASK);

	/* idle device */
	kgsl_idle(device,  KGSL_TIMEOUT_DEFAULT);

	/* define physical memory range accessible by the core */
	kgsl_regwrite(device, device->mmu.reg.mpu_base, mmu->mpu_base);
	kgsl_regwrite(device, device->mmu.reg.mpu_end,
			mmu->mpu_base + mmu->mpu_range);

	/* enable axi interrupts */
	kgsl_regwrite(device, device->mmu.reg.interrupt_mask,
			GSL_MMU_INT_MASK | MH_INTERRUPT_MASK__MMU_PAGE_FAULT);

	/* sub-client MMU lookups require address translation */
	if ((mmu->config & ~0x1) > 0) {

		kgsl_sharedmem_set(&mmu->dummyspace, 0, 0,
				   mmu->dummyspace.size);

		/* TRAN_ERROR needs a 32 byte (32 byte aligned) chunk of memory
		 * to complete transactions in case of an MMU fault. Note that
		 * we'll leave the bottom 32 bytes of the dummyspace for other
		 * purposes (e.g. use it when dummy read cycles are needed
		 * for other blocks */
		kgsl_regwrite(device, device->mmu.reg.tran_error,
						mmu->dummyspace.physaddr + 32);

		if (mmu->defaultpagetable == NULL)
			mmu->defaultpagetable =
				kgsl_mmu_getpagetable(KGSL_MMU_GLOBAL_PT);
		mmu->hwpagetable = mmu->defaultpagetable;

		kgsl_regwrite(device, device->mmu.reg.pt_page,
			      mmu->hwpagetable->base.gpuaddr);
		kgsl_regwrite(device, device->mmu.reg.va_range,
			      (mmu->hwpagetable->va_base |
			      (mmu->hwpagetable->va_range >> 16)));
		status = kgsl_setstate(device, KGSL_MMUFLAGS_TLBFLUSH);
		if (status) {
			KGSL_MEM_ERR(device, "Failed to setstate TLBFLUSH\n");
			goto error;
		}
	}

	return 0;
error:
	/* disable MMU */
	kgsl_regwrite(device, device->mmu.reg.interrupt_mask, 0);
	kgsl_regwrite(device, device->mmu.reg.config, 0x00000000);
	return status;
}



#ifdef CONFIG_MSM_KGSL_MMU

unsigned int kgsl_virtaddr_to_physaddr(unsigned int virtaddr)
{
	unsigned int physaddr = 0;
	pgd_t *pgd_ptr = NULL;
	pmd_t *pmd_ptr = NULL;
	pte_t *pte_ptr = NULL, pte;

	spin_lock(&current->mm->page_table_lock);
	pgd_ptr = pgd_offset(current->mm, virtaddr);
	if (pgd_none(*pgd) || pgd_bad(*pgd)) {
		KGSL_CORE_ERR("Invalid pgd entry\n");
		goto done;
	}

	pmd_ptr = pmd_offset(pgd_ptr, virtaddr);
	if (pmd_none(*pmd_ptr) || pmd_bad(*pmd_ptr)) {
		KGSL_CORE_ERR("Invalid pmd entry\n");
		goto done;
	}

	pte_ptr = pte_offset_map(pmd_ptr, virtaddr);
	if (!pte_ptr) {
		KGSL_CORE_ERR("pt_offset_map failed\n");
		goto done;
	}
	pte = *pte_ptr;
	physaddr = pte_pfn(pte);
	pte_unmap(pte_ptr);
done:
	spin_unlock(&current->mm->page_table_lock);
	physaddr <<= PAGE_SHIFT;
	return physaddr;
}

int
kgsl_mmu_map(struct kgsl_pagetable *pagetable,
				unsigned int address,
				int range,
				unsigned int protflags,
				unsigned int *gpuaddr,
				unsigned int flags)
{
	int numpages;
	unsigned int pte, ptefirst, ptelast, physaddr;
	int flushtlb, alloc_size;
	unsigned int align = flags & KGSL_MEMFLAGS_ALIGN_MASK;

	BUG_ON(protflags & ~(GSL_PT_PAGE_RV | GSL_PT_PAGE_WV));
	BUG_ON(protflags == 0);
	BUG_ON(range <= 0);

	/* Only support 4K and 8K alignment for now */
	if (align != KGSL_MEMFLAGS_ALIGN8K && align != KGSL_MEMFLAGS_ALIGN4K) {
		KGSL_CORE_ERR("invalid flags: %x\n", flags);
		return -EINVAL;
	}

	/* Make sure address being mapped is at 4K boundary */
	if (!IS_ALIGNED(address, PAGE_SIZE) || range & ~PAGE_MASK) {
		KGSL_CORE_ERR("address %x not aligned\n", address);
		return -EINVAL;
	}
	alloc_size = range;
	if (align == KGSL_MEMFLAGS_ALIGN8K)
		alloc_size += PAGE_SIZE;

/// HTC:
	if(alloc_size > MMU_MAP_WARNING_ALLOC_SIZE) {
		KGSL_CORE_ERR("(htc) warning - [%d] alloc extremely large memory, alloc_size = 0x%x, range = 0x%x\n",
							pagetable->name, alloc_size, range);
	}
/// :HTC

	*gpuaddr = gen_pool_alloc(pagetable->pool, alloc_size);
	if (*gpuaddr == 0) {
		KGSL_CORE_ERR("gen_pool_alloc(%d) failed\n", alloc_size);
		KGSL_CORE_ERR(" [%d] allocated=0x%x, entries=0x%x\n",
				pagetable->name, pagetable->stats.mapped,
				pagetable->stats.entries);
		return -ENOMEM;
	}

	if (align == KGSL_MEMFLAGS_ALIGN8K) {
		if (*gpuaddr & ((1 << 13) - 1)) {
			/* Not 8k aligned, align it */
			gen_pool_free(pagetable->pool, *gpuaddr, PAGE_SIZE);
			*gpuaddr = *gpuaddr + PAGE_SIZE;
		} else
			gen_pool_free(pagetable->pool, *gpuaddr + range,
				      PAGE_SIZE);
	}

	numpages = (range >> PAGE_SHIFT);

	ptefirst = kgsl_pt_entry_get(pagetable, *gpuaddr);
	ptelast = ptefirst + numpages;

	pte = ptefirst;
	flushtlb = 0;

	/* tlb needs to be flushed when the first and last pte are not at
	* superpte boundaries */
	if ((ptefirst & (GSL_PT_SUPER_PTE - 1)) != 0 ||
		((ptelast + 1) & (GSL_PT_SUPER_PTE-1)) != 0)
		flushtlb = 1;

	spin_lock(&pagetable->lock);
	for (pte = ptefirst; pte < ptelast; pte++) {
#ifdef VERBOSE_DEBUG
		/* check if PTE exists */
		uint32_t val = kgsl_pt_map_getaddr(pagetable, pte);
		BUG_ON(val != 0 && val != GSL_PT_PAGE_DIRTY);
#endif
		if ((pte & (GSL_PT_SUPER_PTE-1)) == 0)
			if (GSL_TLBFLUSH_FILTER_ISDIRTY(pte / GSL_PT_SUPER_PTE))
				flushtlb = 1;
		/* mark pte as in use */
		if (flags & KGSL_MEMFLAGS_CONPHYS)
			physaddr = address;
		else if (flags & KGSL_MEMFLAGS_VMALLOC_MEM) {
			physaddr = vmalloc_to_pfn((void *)address);
			physaddr <<= PAGE_SHIFT;
			if (physaddr == 0)
				KGSL_CORE_ERR("Unable to map VMEM addr"
					"address: %x\n", address);
		} else if (flags & KGSL_MEMFLAGS_HOSTADDR) {
			physaddr = kgsl_virtaddr_to_physaddr(address);
			if (physaddr == 0)
				KGSL_CORE_ERR("Unable to map hostaddr"
					"address: %x\n", address);
		}else
			physaddr = 0;

		if (physaddr) {
			kgsl_pt_map_set(pagetable, pte, physaddr | protflags);
		} else {
			KGSL_CORE_ERR("Unable to find physaddr for"
				"address: %x\n", address);
			spin_unlock(&pagetable->lock);
			/* Increase the stats here for proper accounting in
			   kgsl_mmu_unmap */
			pagetable->stats.entries += 1;
			pagetable->stats.mapped += alloc_size;

			kgsl_mmu_unmap(pagetable, *gpuaddr, range);
			return -EFAULT;
		}

		address += PAGE_SIZE;
	}

	/* Keep track of the statistics for the sysfs files */

	KGSL_STATS_ADD(1, pagetable->stats.entries,
		       pagetable->stats.max_entries);

	KGSL_STATS_ADD(alloc_size, pagetable->stats.mapped,
		       pagetable->stats.max_mapped);

	mb();

	/* Invalidate tlb only if current page table used by GPU is the
	 * pagetable that we used to allocate */
	if (flushtlb) {
		/*set all devices as needing flushing*/
		pagetable->tlb_flags = UINT_MAX;
		GSL_TLBFLUSH_FILTER_RESET();
	}
	spin_unlock(&pagetable->lock);

	return 0;
}

int
kgsl_mmu_unmap(struct kgsl_pagetable *pagetable, unsigned int gpuaddr,
		int range)
{
	unsigned int numpages;
	unsigned int pte, ptefirst, ptelast, superpte;

	BUG_ON(range <= 0);

	numpages = (range >> PAGE_SHIFT);
	if (range & (PAGE_SIZE - 1))
		numpages++;

	ptefirst = kgsl_pt_entry_get(pagetable, gpuaddr);
	ptelast = ptefirst + numpages;

	spin_lock(&pagetable->lock);
	superpte = ptefirst - (ptefirst & (GSL_PT_SUPER_PTE-1));
	GSL_TLBFLUSH_FILTER_SETDIRTY(superpte / GSL_PT_SUPER_PTE);
	for (pte = ptefirst; pte < ptelast; pte++) {
#ifdef VERBOSE_DEBUG
		/* check if PTE exists */
		BUG_ON(!kgsl_pt_map_getaddr(pagetable, pte));
#endif
		kgsl_pt_map_set(pagetable, pte, GSL_PT_PAGE_DIRTY);
		superpte = pte - (pte & (GSL_PT_SUPER_PTE - 1));
		if (pte == superpte)
			GSL_TLBFLUSH_FILTER_SETDIRTY(superpte /
				GSL_PT_SUPER_PTE);
	}

	/* Remove the statistics */
	pagetable->stats.entries--;
	pagetable->stats.mapped -= range;

	mb();
	spin_unlock(&pagetable->lock);

	gen_pool_free(pagetable->pool, gpuaddr, range);

	return 0;
}
#endif /*CONFIG_MSM_KGSL_MMU*/

int kgsl_mmu_map_global(struct kgsl_pagetable *pagetable,
			struct kgsl_memdesc *memdesc, unsigned int protflags,
			unsigned int flags)
{
	int result = -EINVAL;
	unsigned int gpuaddr = 0;

	if (memdesc == NULL) {
		KGSL_CORE_ERR("invalid memdesc\n");
		goto error;
	}

	result = kgsl_mmu_map(pagetable, memdesc->physaddr, memdesc->size,
				protflags, &gpuaddr, flags);
	if (result)
		goto error;

	/*global mappings must have the same gpu address in all pagetables*/
	if (memdesc->gpuaddr == 0)
		memdesc->gpuaddr = gpuaddr;

	else if (memdesc->gpuaddr != gpuaddr) {
		KGSL_CORE_ERR("pt %p addr mismatch phys 0x%08x"
			"gpu 0x%0x 0x%08x", pagetable, memdesc->physaddr,
			memdesc->gpuaddr, gpuaddr);
		goto error_unmap;
	}
	return result;
error_unmap:
	kgsl_mmu_unmap(pagetable, gpuaddr, memdesc->size);
error:
	return result;
}

int kgsl_mmu_stop(struct kgsl_device *device)
{
	/*
	 *  stop device mmu
	 *
	 *  call this with the global lock held
	 */
	struct kgsl_mmu *mmu = &device->mmu;

	if (mmu->flags & KGSL_FLAGS_STARTED) {
		/* disable mh interrupts */
		/* disable MMU */
		kgsl_regwrite(device, device->mmu.reg.interrupt_mask, 0);
		kgsl_regwrite(device, device->mmu.reg.config, 0x00000000);

		mmu->flags &= ~KGSL_FLAGS_STARTED;
	}

	return 0;
}

int kgsl_mmu_close(struct kgsl_device *device)
{
	/*
	 *  close device mmu
	 *
	 *  call this with the global lock held
	 */
	struct kgsl_mmu *mmu = &device->mmu;

	if (mmu->dummyspace.gpuaddr)
		kgsl_sharedmem_free(&mmu->dummyspace);

	if (mmu->defaultpagetable)
		kgsl_mmu_putpagetable(mmu->defaultpagetable);

	return 0;
}
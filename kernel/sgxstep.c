/*
 *  This file is part of the SGX-Step enclave execution control framework.
 *
 *  Copyright (C) 2017 Jo Van Bulck <jo.vanbulck@cs.kuleuven.be>,
 *                     Raoul Strackx <raoul.strackx@cs.kuleuven.be>
 *
 *  SGX-Step is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  SGX-Step is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with SGX-Step. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sgxstep_internal.h"
#include "sgxstep_ioctl.h"

#include <asm/pgtable.h>
#include <asm/page.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <asm/irq.h>

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>

#include <linux/clockchips.h>
#include <linux/version.h>

#if !NO_SGX
    #include "linux-sgx-driver/sgx.h"
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jo Van Bulck <jo.vanbulck@cs.kuleuven.be>, Raoul Strackx <raoul.strackx@cs.kuleuven.be>");
MODULE_DESCRIPTION("SGX-Step: A Practical Attack Framework for Precise Enclave Execution Control");

int target_cpu = -1;

int step_open(struct inode *inode, struct file *file)
{
    if (target_cpu != -1)
    {   
        err("Device is already opened");
        return -EBUSY;
    }
    target_cpu = smp_processor_id();

    return 0;
}

int step_release(struct inode *inode, struct file *file)
{
    target_cpu = -1;

    return 0;
}

long sgx_step_ioctl_info(struct file *filep, unsigned int cmd, unsigned long arg)
{
    #if !NO_SGX
        struct sgx_encl *enclave;
        struct vm_area_struct *vma = NULL;
        struct sgx_step_enclave_info *info = (struct sgx_step_enclave_info *) arg;

        vma = find_vma(current->mm, (uint64_t) info->tcs);
        RET_ASSERT(vma && (enclave = vma->vm_private_data));
        RET_ASSERT(info->aep && info->tcs);

        info->base = enclave->base;
        info->size = enclave->size;
    #endif

    return 0;
}

long edbgrdwr(unsigned long addr, void *buf, int len, int write)
{
    struct vm_area_struct *vma = NULL;

    /* use the vm_operations defined by the isgx driver
     * (so we don't have to worry about illegal ptrs or #PFs etc) */
    vma = find_vma(current->mm, addr);
    RET_ASSERT(vma && vma->vm_ops && vma->vm_ops->access);
    return vma->vm_ops->access(vma, addr, buf, len, write);
}

long sgx_step_ioctl_edbgrd(struct file *filep, unsigned int cmd, unsigned long arg)
{
    edbgrd_t *data = (edbgrd_t*) arg;
    uint8_t buf[data->len];
    if (data->write && copy_from_user(buf, (void __user *) data->val, data->len))
        return -EFAULT;

    edbgrdwr((unsigned long) data->adrs, &buf, data->len, data->write);

    if (!data->write && copy_to_user((void __user *) data->val, buf, data->len))
        return -EFAULT;
    return 0;
}

/* Convenience function when editing PTEs from user space (but normally not
 * needed, since SGX already flushes the TLB on enclave entry/exit) */
long sgx_step_ioctl_invpg(struct file *filep, unsigned int cmd, unsigned long arg)
{
    uint64_t addr = ((invpg_t *) arg)->adrs;

    asm volatile("invlpg (%0)" ::"r" (addr) : "memory");

    return 0;
}

/* https://elixir.bootlin.com/linux/v5.4.109/source/arch/x86/mm/fault.c#L446 */
void dump_pt(unsigned long address)
{
        pgd_t *base = __va(read_cr3_pa());
	pgd_t *pgd = base + pgd_index(address);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pr_info("PGD %lx ", pgd_val(*pgd));

	if (!pgd_present(*pgd))
		goto out;

	p4d = p4d_offset(pgd, address);

	pr_cont("P4D %lx ", p4d_val(*p4d));
	if (!p4d_present(*p4d) || p4d_large(*p4d))
		goto out;

	pud = pud_offset(p4d, address);

	pr_cont("PUD %lx ", pud_val(*pud));
	if (!pud_present(*pud) || pud_large(*pud))
		goto out;

	pmd = pmd_offset(pud, address);

	pr_cont("PMD %lx ", pmd_val(*pmd));
	if (!pmd_present(*pmd) || pmd_large(*pmd))
		goto out;

	pte = pte_offset_kernel(pmd, address);

	pr_cont("PTE %lx", pte_val(*pte));
out:
	pr_cont("\n");
	return;
}

int fun_kernel(void)
{
    return 0xbadc0de;
}

void do_fun(fun_t f)
{
        printk("fun at %lx with mapping:\n", f);
        dump_pt(f);
        printk("returned %x\n", f());
}

long sgx_step_get_pt_mapping(struct file *filep, unsigned int cmd, unsigned long arg)
{
    address_mapping_t *map = (address_mapping_t*) arg;
	pgd_t *pgd = NULL;
	pud_t *pud = NULL;
	pmd_t *pmd = NULL;
	pte_t *pte = NULL;
    #if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0))
        p4d_t *p4d = NULL;
    #endif

	uint64_t virt;
    RET_ASSERT(map);

        #define CR4_SMEP_MASK      (1 << 20)
        #define CR4_SMAP_MASK      (1 << 21)

        unsigned long val = native_read_cr4();
        printk("cr4 before %lx\n", val);
        val &= ~CR4_SMEP_MASK;
        val &= ~CR4_SMAP_MASK;
        printk("cr4 masked %lx\n", val);
	asm volatile("mov %0,%%cr4": "+r" (val) : : "memory");

        do_fun(fun_kernel);
        do_fun(map->fun);

	virt = map->virt;
	memset( map, 0x00, sizeof( address_mapping_t ) );
	map->virt = virt;

	map->pgd_phys_address = __pa( current->mm->pgd );
	pgd = pgd_offset( current->mm, virt );
	map->pgd = *((uint64_t *) pgd);
	
	if ( !pgd_present( *pgd ) )
		return 0;

    #if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0))
        #if CONFIG_PGTABLE_LEVELS > 4
            #warning 5-level page tables currently not supported by SGX-Step
            #warning unfolding dummy p4d; try rebooting with `no5lvl` kernel parameter if needed
        #endif

        /* simply unfold the pgd inside the dummy p4d struct */
        p4d = p4d_offset( pgd, virt);
        pud = pud_offset( p4d, virt );
    #else
        pud = pud_offset( pgd, virt );
    #endif

	map->pud = *((uint64_t *) pud);
	
	if ( !pud_present( *pud ) )
		return 0;
	
	pmd = pmd_offset( pud, virt );
	map->pmd = *((uint64_t *) pmd);
	
	if ( !pmd_present( *pmd ) )
		return 0;
	
	pte = pte_offset_map( pmd, virt );
	map->pte = *((uint64_t *) pte);
	
	if ( !pte_present( *pte ) )
		return 0;
	
	map->phys = PFN_PHYS( pte_pfn( *pte ) ) | ( virt & 0xfff );

    return 0;
}

typedef long (*ioctl_t)(struct file *filep, unsigned int cmd, unsigned long arg);

long step_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
    char data[256];
    ioctl_t handler = NULL;
    long ret;

    switch (cmd)
    {
        case SGX_STEP_IOCTL_VICTIM_INFO:
            handler = sgx_step_ioctl_info;
            break;
        case SGX_STEP_IOCTL_GET_PT_MAPPING:
            handler = sgx_step_get_pt_mapping;
            break;
        case SGX_STEP_IOCTL_EDBGRD:
            handler = sgx_step_ioctl_edbgrd;
            break;
        case SGX_STEP_IOCTL_INVPG:
            handler = sgx_step_ioctl_invpg;
            break;
        default:
            return -EINVAL;
    }

    RET_ASSERT(handler && (_IOC_SIZE(cmd) < 256));
    if (copy_from_user(data, (void __user *) arg, _IOC_SIZE(cmd)))
        return -EFAULT;

    ret = handler(filep, cmd, (unsigned long) ((void *) data));

    if (!ret && (cmd & IOC_OUT)) {
        if (copy_to_user((void __user *) arg, data, _IOC_SIZE(cmd)))
            return -EFAULT;
    }

    return 0;
}

static const struct file_operations step_fops = {
    .owner              = THIS_MODULE,
    .compat_ioctl       = step_ioctl,
    .unlocked_ioctl     = step_ioctl,
    .open               = step_open,
    .release            = step_release
};

static struct miscdevice step_dev = {
    .minor  = MISC_DYNAMIC_MINOR,
    .name   = DEV,
    .fops   = &step_fops,
    .mode   = S_IRUGO | S_IWUGO
};

/* Code from: <https://www.libcrack.so/index.php/2012/09/02/bypassing-devmem_is_allowed-with-kprobes/> */
static int devmem_is_allowed_handler (struct kretprobe_instance *rp, struct pt_regs *regs)
{
    if (regs->ax == 0) {
        regs->ax = 0x1;
    }
    return 0;
}

static struct kretprobe krp = {
    .handler = devmem_is_allowed_handler,
    .maxactive = 20 /* Probe up to 20 instances concurrently. */
};

int init_module(void)
{
    /* Register virtual device */
    if (misc_register(&step_dev))
    {
        err("virtual device registration failed..");
        step_dev.this_device = NULL;
        return -EINVAL;
    }

    /* Activate a kretprobe to bypass CONFIG_STRICT_DEVMEM kernel compilation option */
    krp.kp.symbol_name = "devmem_is_allowed";
    if (register_kretprobe(&krp) < 0)
    {
        err("register_kprobe failed..");
        step_dev.this_device = NULL;
        return -EINVAL;
    }

    log("listening on /dev/" DEV);
    return 0;
}

void cleanup_module(void)
{
    /* Unregister virtual device */
    if (step_dev.this_device)
        misc_deregister(&step_dev);


    unregister_kretprobe(&krp);
    log("kernel module unloaded");
}

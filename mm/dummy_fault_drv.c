#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/gfp.h>

#define HUGE_PMD_ORDER 9
#define HUGE_PMD_SIZE (1UL << 21) // 2MB

static struct page *dummy_page;

/* Linux 7.0 现代 API：处理大页缺页异常 */
static vm_fault_t dummy_huge_fault(struct vm_fault *vmf, unsigned int order)
{
    if (order != PMD_ORDER)
        return VM_FAULT_FALLBACK;

    unsigned long pfn = page_to_pfn(dummy_page);
    
    /* 
     * 核心复现点：调用 vmf_insert_pfn_pmd
     * 内部会调用 insert_pmd，打上 special 标记，但不 deposit 页表 (x86上)
     */
    return vmf_insert_pfn_pmd(vmf, pfn, vmf->flags & FAULT_FLAG_WRITE);
}

static const struct vm_operations_struct dummy_vm_ops = {
    .huge_fault = dummy_huge_fault,
    /* 普通 fault fallback，以防万一 */
    .fault = NULL, 
};

static int dummy_fault_mmap(struct file *file, struct vm_area_struct *vma)
{
    /* 设置 PFNMAP，同时设置 HUGEPAGE 鼓励内核触发 huge_fault */
    vm_flags_set(vma, VM_PFNMAP | VM_HUGEPAGE);
    vma->vm_ops = &dummy_vm_ops;
    return 0;
}

static const struct file_operations dummy_fault_fops = {
    .owner = THIS_MODULE,
    .mmap = dummy_fault_mmap,
};

static struct miscdevice dummy_fault_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "dummy_fault_pfn",
    .fops = &dummy_fault_fops,
};

static int __init dummy_fault_init(void)
{
    dummy_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, HUGE_PMD_ORDER);
    if (!dummy_page)
        return -ENOMEM;
    return misc_register(&dummy_fault_misc);
}

static void __exit dummy_fault_exit(void)
{
    misc_deregister(&dummy_fault_misc);
    __free_pages(dummy_page, HUGE_PMD_ORDER);
}

module_init(dummy_fault_init);
module_exit(dummy_fault_exit);
MODULE_LICENSE("GPL");

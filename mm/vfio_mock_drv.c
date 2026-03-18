#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/huge_mm.h>
#include <linux/log2.h>
#include <linux/pgtable.h> // 需要用到 PMD_SHIFT

#define MOCK_MEM_SIZE (2 * 1024 * 1024) // 2MB PMD Size

static struct page *mock_hw_pages; // 【优化】直接保存 struct page

// 4K PTE 缺页回退处理
static vm_fault_t mock_vfio_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    
    // 🚨 核心修复 2 (MM 最精髓的偏移量计算)：
    // VMA 被 munmap 切掉前 4K 后，内核会自动将新 VMA 的 vm_pgoff 加 1。
    // 所以物理内存的映射偏移，必须是 vm_pgoff 加上相对当前 VMA 的 offset！
    unsigned long vma_page_offset = vma->vm_pgoff + ((vmf->address - vma->vm_start) >> PAGE_SHIFT);
    unsigned long pfn = page_to_pfn(mock_hw_pages) + vma_page_offset;

    pr_info("[VFIO-Mock] 4K PTE Fault: vaddr=0x%lx, pgoff=%lu, pfn=0x%lx\n", 
            vmf->address, vma_page_offset, pfn);
    
    return vmf_insert_pfn(vma, vmf->address, pfn);
}

// 2M PMD 缺页处理
static vm_fault_t mock_vfio_huge_fault(struct vm_fault *vmf, unsigned int order)
{
    struct vm_area_struct *vma = vmf->vma;
    unsigned long pfn = page_to_pfn(mock_hw_pages);
    bool write = vmf->flags & FAULT_FLAG_WRITE;

    if (order != (PMD_SHIFT - PAGE_SHIFT))
        return VM_FAULT_FALLBACK;

    // 🚨 核心修复 1 (工业级防线)：
    // 检查当前的 VMA 是不是一个完整的、对齐的 2M 区间！
    // PMD_MASK 在 x86_64 通常是 ~(2M - 1)
    if ((vma->vm_start & ~PMD_MASK) || (vma->vm_end & ~PMD_MASK)) {
        pr_info("[VFIO-Mock] VMA is split(start=0x%lx)，degrade to PTE!\n", vma->vm_start);
        
        // 返回 FALLBACK 告诉内核 MM："我处理不了大页了，请用 4K 的 .fault 回调重试！"
        return VM_FAULT_FALLBACK; 
    }

    pr_info("[VFIO-Mock] 2M PMD Fault: vaddr=0x%lx, pfn=0x%lx\n", vmf->address, pfn);

    return vmf_insert_pfn_pmd(vmf, pfn, write);
}

static const struct vm_operations_struct mock_vm_ops = {
    .fault      = mock_vfio_fault,
    .huge_fault = mock_vfio_huge_fault,
};

static int mock_mmap(struct file *file, struct vm_area_struct *vma)
{
    size_t size = vma->vm_end - vma->vm_start;

    if (size != MOCK_MEM_SIZE) {
        pr_err("[VFIO-Mock] can only support 2MB map\n");
        return -EINVAL;
    }

    // 【Linux 6.3+ API 适配】必须通过 vm_flags_set 设置
    vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_HUGEPAGE);
    
    vma->vm_ops = &mock_vm_ops;
    
    pr_info("[VFIO-Mock] mmap 完成，等待缺页异常按需建表...\n");
    return 0;
}

static const struct file_operations mock_fops = {
    .owner = THIS_MODULE,
    .mmap  = mock_mmap,
};

static struct miscdevice mock_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "vfio_mock",
    .fops  = &mock_fops,
};

static int __init mock_init(void)
{
    // 【优化】直接申请 Order 9 (2M) 的物理页，这是最地道的 MM 做法
    mock_hw_pages = alloc_pages(GFP_KERNEL | __GFP_COMP | __GFP_ZERO, get_order(MOCK_MEM_SIZE));
    if (!mock_hw_pages)
        return -ENOMEM;
        
    pr_info("[VFIO-Mock] 物理内存分配成功: pfn=0x%lx\n", page_to_pfn(mock_hw_pages));
    return misc_register(&mock_miscdev);
}

static void __exit mock_exit(void)
{
    misc_deregister(&mock_miscdev);
    __free_pages(mock_hw_pages, get_order(MOCK_MEM_SIZE));
    pr_info("[VFIO-Mock] uninstalled.\n");
}

module_init(mock_init);
module_exit(mock_exit);
MODULE_LICENSE("GPL");

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/fs.h>

#define HUGE_PMD_ORDER 9
#define HUGE_PMD_SIZE (1UL << 21) // 2MB

static struct page *dummy_page;

static int dummy_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;

    /* 我们只允许映射 2MB 的大小测试 */
    if (size != HUGE_PMD_SIZE)
        return -EINVAL;

    pfn = page_to_pfn(dummy_page);

    /* 调用你的 remap_pfn_range，如果你的补丁生效，这里会建立 PMD 映射 */
    return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
}

static const struct file_operations dummy_fops = {
    .owner = THIS_MODULE,
    .mmap = dummy_mmap,
};

static struct miscdevice dummy_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "dummy_pfn",
    .fops = &dummy_fops,
};

static int __init dummy_init(void)
{
    /* 分配 order 9 (2MB) 的物理连续页面，天然 2M 对齐 */
    dummy_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, HUGE_PMD_ORDER);
    if (!dummy_page)
        return -ENOMEM;
    
    return misc_register(&dummy_misc);
}

static void __exit dummy_exit(void)
{
    misc_deregister(&dummy_misc);
    if (dummy_page)
        __free_pages(dummy_page, HUGE_PMD_ORDER);
}

module_init(dummy_init);
module_exit(dummy_exit);
MODULE_LICENSE("GPL");

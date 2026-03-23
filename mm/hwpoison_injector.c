#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/kallsyms.h>

static struct task_struct *poison_tsk;
// 使用原子变量保证无锁极速传递，抢占时间窗口
static atomic_long_t target_pfn = ATOMIC_LONG_INIT(0);

// 定义 memory_failure 的函数指针
// 注意：较新内核中 memory_failure 可能没有 EXPORT_SYMBOL，需要动态解析
static int (*my_memory_failure)(unsigned long pfn, int flags);

/*
 * 后台“刺客”线程：不休眠，疯狂自旋轮询
 */
static int poison_thread(void *data)
{
    while (!kthread_should_stop()) {
        // 原子地读出并清零 target_pfn
        unsigned long pfn = atomic_long_xchg(&target_pfn, 0);
        
        if (pfn) {
            pr_emerg("[Injector] Intercepted Folio PFN 0x%lx! Firing memory_failure NOW!\n", pfn);
            // 模拟硬件 ECC 错误，强行拆解该大页！
            if (my_memory_failure)
                my_memory_failure(pfn, 0); // 0 代表正常 flag
        }
        // 千万不要用 usleep，要用 cpu_relax 疯狂空转，
        // 只有这样才能跑赢 init_new_hugetlb_folio 的执行速度！
        cpu_relax();
    }
    return 0;
}

/*
 * Kretprobe 返回拦截器
 * 此时 alloc_buddy_frozen_folio 刚好执行完，准备返回 folio 指针
 */
static int alloc_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    // 从寄存器 (ARM64 的 x0) 中提取返回值
    struct folio *folio = (struct folio *)regs_return_value(regs);
    
    if (folio) {
        // 算出刚刚分配出来的大页的 PFN
        unsigned long pfn = folio_pfn(folio);
        
        // 瞬间塞给后台刺客线程
        atomic_long_set(&target_pfn, pfn);
    }
    return 0;
}

static struct kretprobe my_kretprobe = {
    .handler = alloc_ret_handler,
    .kp.symbol_name = "alloc_buddy_frozen_folio",
};

static int __init hwpoison_injector_init(void)
{
    int ret;

    // 1. 动态解析 memory_failure 地址 (规避未 EXPORT 的问题)
    // 如果你的内核版本 kallsyms_lookup_name 被移除了，可以使用 kprobe 来获取地址
    my_memory_failure = (void *)kallsyms_lookup_name("memory_failure");
    if (!my_memory_failure) {
        pr_err("[Injector] Cannot find memory_failure address!\n");
        return -EINVAL;
    }

    // 2. 启动后台自旋线程
    poison_tsk = kthread_run(poison_thread, NULL, "poison_injector");
    if (IS_ERR(poison_tsk))
        return PTR_ERR(poison_tsk);

    // 3. 挂载 Kretprobe 拦截器
    ret = register_kretprobe(&my_kretprobe);
    if (ret < 0) {
        pr_err("[Injector] register_kretprobe failed, returned %d\n", ret);
        kthread_stop(poison_tsk);
        return ret;
    }

    pr_info("[Injector] Loaded successfully. Ready to assassinate folios!\n");
    return 0;
}

static void __exit hwpoison_injector_exit(void)
{
    unregister_kretprobe(&my_kretprobe);
    if (poison_tsk)
        kthread_stop(poison_tsk);
    pr_info("[Injector] Unloaded.\n");
}

module_init(hwpoison_injector_init);
module_exit(hwpoison_injector_exit);
MODULE_LICENSE("GPL");

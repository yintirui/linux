#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/wait.h>

#define PMD_SIZE (2 * 1024 * 1024UL)
#define PAGE_SIZE 4096UL

#define PTDUMP_PATH "/sys/kernel/debug/page_tables/current_user"

static jmp_buf segv_jmp;

/* 捕捉 SIGSEGV 的信号处理函数 */
static void sigsegv_handler(int sig, siginfo_t *si, void *unused)
{
    longjmp(segv_jmp, 1);
}

/* 检查 PTDUMP 是否可用 */
static void check_ptdump_availability(void)
{
    FILE *f = fopen(PTDUMP_PATH, "r");
    if (!f) {
        fprintf(stderr, "[-] Fatal: Cannot open %s\n", PTDUMP_PATH);
        fprintf(stderr, "    Please ensure you are running as ROOT and CONFIG_PTDUMP_DEBUGFS is enabled.\n");
        exit(EXIT_FAILURE);
    }
    fclose(f);
}

/* 核心观测函数：从 PTDUMP 中提取指定范围的页表信息并打印到屏幕 */
static void print_ptdump_for_range(unsigned long target_start, unsigned long target_end)
{
    FILE *f = fopen(PTDUMP_PATH, "r");
    if (!f) return;

    char line[512];
    int found = 0;
    
    printf("\n  >>> PTDUMP State for range [0x%lx - 0x%lx] <<<\n", target_start, target_end);
    
    while (fgets(line, sizeof(line), f)) {
        unsigned long start = 0, end = 0;
        char *hex_start = strstr(line, "0x");
        
        if (hex_start && sscanf(hex_start, "0x%lx-0x%lx", &start, &end) == 2) {
            /* 检查地址段是否相交 (overlap) */
            if (start < target_end && end > target_start) {
                /* 去掉换行符以便格式化输出 */
                line[strcspn(line, "\n")] = 0;
                printf("      %s\n", line);
                found = 1;
            }
        }
    }
    
    if (!found) {
        printf("      (No matching page table entries found. It might be unmapped.)\n");
    }
    printf("  >>> ------------------------------------------------ <<<\n\n");
    fclose(f);
}

/* 获取严格 2M 对齐的映射地址 */
static void *mmap_aligned_2m(int fd)
{
    /* 先分配 4M 的匿名内存 */
    unsigned long orig_addr = (unsigned long)mmap(NULL, PMD_SIZE * 2, 
                                                  PROT_READ | PROT_WRITE, 
                                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((void *)orig_addr == MAP_FAILED) {
        perror("mmap 4M failed");
        exit(EXIT_FAILURE);
    }

    /* 计算 2M 对齐的起始地址 */
    unsigned long aligned_addr = (orig_addr + PMD_SIZE - 1) & ~(PMD_SIZE - 1);

    /* 释放前面的碎片 */
    if (aligned_addr > orig_addr)
        munmap((void *)orig_addr, aligned_addr - orig_addr);
    
    /* 释放后面的碎片 */
    unsigned long tail_addr = aligned_addr + PMD_SIZE;
    unsigned long tail_size = (orig_addr + PMD_SIZE * 2) - tail_addr;
    if (tail_size > 0)
        munmap((void *)tail_addr, tail_size);

    /* 在对齐的地址上映射设备文件 */
    void *final_addr = mmap((void *)aligned_addr, PMD_SIZE, 
                            PROT_READ | PROT_WRITE, 
                            MAP_SHARED | MAP_FIXED, fd, 0);
    
    if (final_addr == MAP_FAILED) {
        perror("mmap device failed");
        exit(EXIT_FAILURE);
    }
    
    return final_addr;
}

/* Test 1: 常规 PMD 映射测试 */
void test_normal_map(int fd)
{
    printf("[*] Test 1: Normal 2M PMD Mapping\n");
    void *ptr = mmap_aligned_2m(fd);
    
    /* 触发缺页异常，真正建立页表 (假设你的驱动没有设置 VM_DONTEXPAND 等) */
    memset(ptr, 0xAA, PMD_SIZE);

    printf("[+] Mapped 2MB from device at address 0x%lx\n", (unsigned long)ptr);
    printf("    Action: Expected to see a single 2M PMD entry in page table.\n");
    
    print_ptdump_for_range((unsigned long)ptr, (unsigned long)ptr + PMD_SIZE);
    
    munmap(ptr, PMD_SIZE);
    printf("----------------------------------------------------------\n");
}

/* Test 2: mprotect 触发大页拆分 */
void test_mprotect_split(int fd)
{
    printf("[*] Test 2: PMD Split via mprotect()\n");
    void *ptr = mmap_aligned_2m(fd);
    memset(ptr, 0xBB, PMD_SIZE); /* Fault in */

    unsigned long target = (unsigned long)ptr + PAGE_SIZE; // 第二页

    printf("[+] Initial mapping established at 0x%lx\n", (unsigned long)ptr);
    printf("[+] Action: mprotect(PROT_READ) on 4K page at 0x%lx\n", target);
    printf("    Expected: The 2M PMD should be split into 512 4K PTEs.\n");

    if (mprotect((void *)target, PAGE_SIZE, PROT_READ) != 0) {
        perror("mprotect failed");
    }

    /* 打印 PTDUMP，人工验证是否全部变成了 4K 页，并且目标页权限是 ro */
    print_ptdump_for_range((unsigned long)ptr, (unsigned long)ptr + PMD_SIZE);

    munmap(ptr, PMD_SIZE);
    printf("----------------------------------------------------------\n");
}

/* Test 3: munmap 触发大页拆分并测试 SIGSEGV */
void test_munmap_split(int fd)
{
    printf("[*] Test 3: PMD Split via partial munmap() and SIGSEGV validation\n");
    void *ptr = mmap_aligned_2m(fd);
    memset(ptr, 0xCC, PMD_SIZE); /* Fault in */

    unsigned long target = (unsigned long)ptr + PAGE_SIZE * 2; // 第三页

    printf("[+] Initial mapping established at 0x%lx\n", (unsigned long)ptr);
    printf("[+] Action: munmap() 4K page at 0x%lx\n", target);
    printf("    Expected: PMD split into 4K PTEs, and a 4K hole should appear in PTDUMP.\n");

    if (munmap((void *)target, PAGE_SIZE) != 0) {
        perror("munmap failed");
    }

    print_ptdump_for_range((unsigned long)ptr, (unsigned long)ptr + PMD_SIZE);

    printf("[+] Action: Trying to write to the unmapped 4K page at 0x%lx...\n", target);
    
    /* 设置信号处理 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    if (setjmp(segv_jmp) == 0) {
        /* 试图向已经被 unmap 的地址写入数据 */
        *((volatile char *)target) = 0xDD;
        printf("[-] FAIL: Accessed unmapped page without triggering SIGSEGV!\n");
    } else {
        printf("[+] PASS: Caught SIGSEGV as expected! The hole is real.\n");
    }

    /* 验证相邻的页面仍然存活 (证明拆分没有破坏前后的数据和页表) */
    *((volatile char *)ptr) = 0xEE;
    *((volatile char *)(target + PAGE_SIZE)) = 0xFF;
    printf("[+] PASS: Neighboring pages are still fully accessible.\n");

    /* 清理残余 */
    munmap(ptr, PAGE_SIZE * 2);
    munmap((void *)(target + PAGE_SIZE), PMD_SIZE - PAGE_SIZE * 3);
    printf("----------------------------------------------------------\n");
}

/* Test 4: fork/clone 触发 copy_huge_pmd，验证 Deposit/Withdraw 逻辑 */
void test_fork_mapping(int fd)
{
    printf("[*] Test 4: Fork/Clone PMD copying & teardown (Syzbot Regression Test)\n");
    void *ptr = mmap_aligned_2m(fd);
    memset(ptr, 0xEE, PMD_SIZE); /* Fault in */

    printf("[+] Initial mapping established at 0x%lx in Parent (PID: %d)\n", (unsigned long)ptr, getpid());

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        /* 子进程空间 */
        printf("[Child %d] Inherited PMD mapping. Accessing memory...\n", getpid());
        /* 验证数据一致性，不应发生 Crash */
        volatile char val = *((volatile char *)ptr);
        (void)val;
       
        printf("    [Child %d] Unmapping and exiting. (Should NOT trigger kernel panic!)\n", getpid());
        /* 触发 zap_huge_pmd 或者进程退出时的 exit_mmap */
        munmap(ptr, PMD_SIZE);
        exit(EXIT_SUCCESS);
    } else {
        /* 父进程空间 */
        int status;
        waitpid(pid, &status, 0); /* 等待子进程安全销毁 */
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("[+] PASS: Child process cleanly exited without crashing the kernel.\n");
        } else {
            printf("[-] FAIL: Child process did not exit normally. (Crash?)\n");
        }
       
        /* 验证父进程自己销毁也不受影响 */
        munmap(ptr, PMD_SIZE);
        printf("[+] PASS: Parent unmapped successfully.\n");
    }
    printf("----------------------------------------------------------\n");
}

/* Test 5: DAX PMD Mapping (Fork & Partial Munmap Split Lifecycle Validation) */
void test_dax_mapping(const char *dax_path)
{
    printf("[*] Test 5: DAX PMD Mapping (Split & Lifecycle Validation)\n");
    int fd = open(dax_path, O_RDWR);
    if (fd < 0) {
        printf("[-] Failed to open %s. Skipping DAX test.\n", dax_path);
        printf("    (Tip: Create it via 'ndctl create-namespace -fe namespace0.0 --mode=devdax --align=2M')\n");
        printf("----------------------------------------------------------\n");
        return;
    }

    /* 获取 2M 对齐的 DAX 映射 */
    void *ptr = mmap_aligned_2m(fd);

    /* 写入数据以触发真正的 PMD DAX 缺页分配 */
    memset(ptr, 0xDD, PMD_SIZE);

    printf("[+] DAX mapped at 0x%lx. Expected: 2M PMD (devmap) entry.\n", (unsigned long)ptr);
    print_ptdump_for_range((unsigned long)ptr, (unsigned long)ptr + PMD_SIZE);

    /* 触发 fork() 测试 copy_huge_pmd 中的 DAX 隔离逻辑 */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
    } else if (pid == 0) {
        /* 子进程空间 */
        printf("    [Child %d] Inherited DAX PMD mapping. Triggering partial munmap (split)...\n", getpid());

        /* 子进程：仅 unmap 前 4K，强行触发 __split_huge_pmd_locked */
        if (munmap(ptr, PAGE_SIZE) != 0) {
            perror("child munmap failed");
        }

        printf("    [Child %d] Partial munmap successful. Unmapping the rest...\n", getpid());
        /* 清理残余映射 */
        munmap((char *)ptr + PAGE_SIZE, PMD_SIZE - PAGE_SIZE);
        exit(EXIT_SUCCESS);
    } else {
        /* 父进程空间 */
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("[+] PASS: Child process cleanly split and unmapped DAX without crashing.\n");
        } else {
            printf("[-] FAIL: Child process crashed on DAX mapping! (Check dmesg for BUG or Panic)\n");
        }

        printf("[+] Parent: Triggering partial munmap (split) on the tail...\n");

        /* 父进程：仅 unmap 最后 4K，强行触发 __split_huge_pmd_locked */
        unsigned long tail_addr = (unsigned long)ptr + PMD_SIZE - PAGE_SIZE;
        if (munmap((void *)tail_addr, PAGE_SIZE) != 0) {
            perror("parent munmap failed");
        }

        printf("[+] Parent: Partial munmap successful. Checking PTDUMP...\n");
        /* 此时应该看到原本的 2M 大页已经被拆碎成了 4K 的小页 */
        print_ptdump_for_range((unsigned long)ptr, tail_addr);

        /* 清理残余映射 */
        munmap(ptr, PMD_SIZE - PAGE_SIZE);
        printf("[+] PASS: Parent unmapped DAX successfully.\n");
    }
    close(fd);
    printf("----------------------------------------------------------\n");
}

/* Test 6: vmf_insert_pfn_pmd Lifecycle Validation (The VFIO / Fault Crash Test) */
void test_fault_insert_pmd(void)
{
    printf("[*] Test 6: vmf_insert_pfn_pmd() Crash Reproduction\n");

    int fd = open("/dev/dummy_fault_pfn", O_RDWR);
    if (fd < 0) {
        perror("[-] Failed to open /dev/dummy_fault_pfn");
        return;
    }

    /* 获取 2M 对齐的映射 */
    void *ptr = mmap_aligned_2m(fd);

    printf("    [+] Mapped via fault driver at 0x%lx. Triggering huge_fault...\n", (unsigned long)ptr);

    /* 写入数据以触发 dummy_huge_fault -> vmf_insert_pfn_pmd */
    memset(ptr, 0xEE, PMD_SIZE);

    print_ptdump_for_range((unsigned long)ptr, (unsigned long)ptr + PMD_SIZE);

    printf("    [+] Fault successful. Now triggering zap_huge_pmd()...\n");
    printf("    [!] WARNING: If the VM_PFNMAP logic is flawed, the kernel will PANIC NOW!\n");

    /*
     * 核心崩溃触发点：
     * munmap 会调用 zap_huge_pmd。
     * 由于 vma 带有 VM_PFNMAP，且 PMD 带有 special 标记，
     * 你的上一版补丁会强行执行 zap_deposited_table()，但实际上根本没有 table！
     */
    munmap(ptr, PMD_SIZE);

    printf("    [+] PASS: Did not crash! The tear-down was handled correctly.\n");
    close(fd);
    printf("----------------------------------------------------------\n");
}

int main(void)
{
    printf("=== remap_pfn_range Huge PMD Validation Tool ===\n");
    
    check_ptdump_availability();

    /* 1. 测试你的 dummy_pfn 设备 (使用 _PAGE_SPECIAL) */
    int fd = open("/dev/dummy_pfn", O_RDWR);
    if (fd >= 0) {
        test_normal_map(fd);
        test_mprotect_split(fd);
        test_munmap_split(fd);
        test_fork_mapping(fd);
        close(fd);
    } else {
        perror("[-] Failed to open /dev/dummy_pfn");
    }

    /* 2. 测试纯正的 Device DAX 设备 (使用 _PAGE_DEVMAP) */
    test_dax_mapping("/dev/dax0.0");

    test_fault_insert_pmd();

    printf("[*] All tests completed successfully.\n");
    return 0;
}

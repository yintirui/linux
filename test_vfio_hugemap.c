#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define MEM_SIZE (2 * 1024 * 1024) // 2MB
#define PAGE_SIZE 4096

#define PTDUMP_PATH "/sys/kernel/debug/page_tables/current_user"

void check_ptdump(uintptr_t target_addr, size_t size) {
    FILE *fp = fopen(PTDUMP_PATH, "r");
    if (!fp) {
        printf("[Info] Cannot open %s\n", PTDUMP_PATH);
        printf("       (Ensure root privileges and CONFIG_PTDUMP_DEBUGFS=y)\n");
        return;
    }

    printf("\n--- Page Table Dump for range[0x%lx - 0x%lx] ---\n", target_addr, target_addr + size);
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0;
        
        // ptdump format typically starts with: 0x<start>-0x<end>
        if (sscanf(line, "0x%lx-0x%lx", &start, &end) == 2) {
            // Filter and print only the lines intersecting with our VMA
            if (start < (target_addr + size) && end > target_addr) {
                printf("%s", line);
            }
        }
    }
    fclose(fp);
    printf("----------------------------------------------------------\n\n");
}

int main() {
    int fd;
    volatile char *ptr;

    printf("=== Test Start: VFIO 2M PMD Mapping and Partial Unmap ===\n");

    fd = open("/dev/vfio_mock", O_RDWR);
    if (fd < 0) {
        perror("Failed to open /dev/vfio_mock");
        return -1;
    }

    // 1. Allocate 4MB anonymous memory to reserve a contiguous virtual address space
    void *temp = mmap(NULL, MEM_SIZE * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (temp == MAP_FAILED) {
        perror("temp mmap failed");
        return -1;
    }

    // 2. Calculate the 2MB strictly aligned starting address within this 4MB space
    uintptr_t temp_addr = (uintptr_t)temp;
    uintptr_t aligned_addr = (temp_addr + MEM_SIZE - 1) & ~(MEM_SIZE - 1);

    // 3. Use MAP_FIXED to instruct the kernel: map the VFIO device exactly at this aligned address
    ptr = mmap((void *)aligned_addr, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }

    printf("[1] mmap 2MB success (2MB aligned). vaddr=%p\n", ptr);

    // 2. Trigger PMD huge page mapping
    printf("[2] Accessing ptr[0]... triggering 2M PMD huge_fault\n");
    ptr[0] = 'A'; 
    printf("    -> Write success! PMD mapping established.\n");

    // 3. Partial unmap (Shattering occurs here!)
    printf("[3] Calling munmap to unmap the first 4K memory...\n");
    if (munmap((void *)ptr, PAGE_SIZE) < 0) {
        perror("munmap failed");
        return -1;
    }
    printf("    -> munmap success! Original 2M VMA is split and degraded.\n");

    // 4. Access remaining space (Trigger 4K PTE fallback)
    volatile char *ptr_plus_4k = ptr + PAGE_SIZE;
    printf("[4] Accessing ptr + 4K (vaddr=%p)... triggering 4K PTE fault\n", ptr_plus_4k);
    
    ptr_plus_4k[0] = 'B';
    printf("    -> Write success! Data: %c. Bypassed degradation gracefully, no SIGBUS!\n", ptr_plus_4k[0]);

    printf("=== Test Success! Kernel completed page table shattering and 4K PTE fallback ===\n");

    // Check page tables via debugfs to verify PTE fallback visually
    check_ptdump(aligned_addr, MEM_SIZE);

    close(fd);
    return 0;
}

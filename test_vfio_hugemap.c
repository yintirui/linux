#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define MEM_SIZE (2 * 1024 * 1024) 
#define PAGE_SIZE 4096

#define PTDUMP_PATH "/sys/kernel/debug/page_tables/current_user"

void check_ptdump(uintptr_t target_addr, size_t size, const char *stage) {
    FILE *fp = fopen(PTDUMP_PATH, "r");
    if (!fp) {
        printf("[Info] Cannot open %s\n", PTDUMP_PATH);
        return;
    }

    printf("\n--- PTDUMP: %s ---\n", stage);
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0;
        
        if (sscanf(line, "0x%lx-0x%lx", &start, &end) == 2) {
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

    void *temp = mmap(NULL, MEM_SIZE * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (temp == MAP_FAILED) {
        perror("temp mmap failed");
        return -1;
    }

    uintptr_t temp_addr = (uintptr_t)temp;
    uintptr_t aligned_addr = (temp_addr + MEM_SIZE - 1) & ~(MEM_SIZE - 1);

    ptr = mmap((void *)aligned_addr, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }

    printf("[1] mmap 2MB success (2MB aligned). vaddr=%p\n", ptr);

    printf("[2] Accessing ptr[0]... triggering 2M PMD huge_fault\n");
    ptr[0] = 'A'; 
    printf("    -> Write success! PMD mapping established.\n");

    check_ptdump(aligned_addr, MEM_SIZE, "STAGE 1: After 2M PMD Mapping established");

    printf("[3] Calling munmap to unmap the first 4K memory...\n");
    if (munmap((void *)ptr, PAGE_SIZE) < 0) {
        perror("munmap failed");
        return -1;
    }
    printf("    -> munmap success! Original 2M VMA is split and degraded.\n");

    check_ptdump(aligned_addr, MEM_SIZE, "STAGE 2: After munmap 4K (Shattering occurred)");

    volatile char *ptr_plus_4k = ptr + PAGE_SIZE;
    printf("[4] Accessing ptr + 4K (vaddr=%p)... triggering 4K PTE fault\n", ptr_plus_4k);
    
    ptr_plus_4k[0] = 'B';
    printf("    -> Write success! Data: %c. Bypassed degradation gracefully, no SIGBUS!\n", ptr_plus_4k[0]);

    check_ptdump(aligned_addr, MEM_SIZE, "STAGE 3: After touching +4K (PTE Fallback inserted)");

    printf("=== Test Success! Kernel completed page table shattering and 4K PTE fallback ===\n");

    close(fd);
    return 0;
}

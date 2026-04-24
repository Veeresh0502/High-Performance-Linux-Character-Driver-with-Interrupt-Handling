#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main() {
    int fd;
    char *mapped_mem;
    size_t map_size = 4096;  /* One page (PAGE_SIZE) */
    
    printf("=== MMAP Test Program ===\n\n");
    
    /* Open device */
    fd = open("/dev/mychardev", O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }
    printf("Device opened successfully\n\n");
    
    /* Test 1: Map kernel buffer to userspace */
    printf("--- Test 1: Map kernel memory ---\n");
    printf("Calling mmap() to map %zu bytes...\n", map_size);
    
    mapped_mem = mmap(NULL,              /* Let kernel choose address */
                     map_size,           /* Size to map */
                     PROT_READ | PROT_WRITE,  /* Read and write */
                     MAP_SHARED,         /* Share with other processes */
                     fd,                 /* Our device file */
                     0);                 /* Offset = 0 */
    
    if (mapped_mem == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return 1;
    }
    
    printf("mmap() succeeded!\n");
    printf("Mapped address: %p\n", mapped_mem);
    printf("Mapped size: %zu bytes\n\n", map_size);
    
    /* Test 2: Read initial content */
    printf("--- Test 2: Read initial content ---\n");
    printf("Initial buffer content: '%s'\n\n", mapped_mem);
    
    /* Test 3: Write to mapped memory (zero-copy!) */
    printf("--- Test 3: Write via mmap (zero-copy) ---\n");
    strcpy(mapped_mem, "Hello from userspace via MMAP! This is zero-copy access.");
    printf("Wrote: '%s'\n", mapped_mem);
    printf("No system call needed - direct memory access!\n\n");
    
    /* Test 4: Verify by reading again */
    printf("--- Test 4: Verify data ---\n");
    printf("Buffer content: '%s'\n\n", mapped_mem);
    
    /* Test 5: Demonstrate zero-copy performance */
    printf("--- Test 5: Performance demo ---\n");
    for (int i = 0; i < 1000; i++) {
        mapped_mem[i % map_size] = 'A' + (i % 26);
    }
    printf("Wrote 1000 bytes with NO system calls (direct memory access)\n");
    printf("First 50 bytes: %.50s...\n\n", mapped_mem);
    
    /* Test 6: Compare with traditional read() */
    printf("--- Test 6: Traditional read() for comparison ---\n");
    char read_buf[100] = {0};
    lseek(fd, 0, SEEK_SET);
    ssize_t bytes = read(fd, read_buf, sizeof(read_buf));
    printf("read() returned %ld bytes (requires system call + copy)\n", bytes);
    printf("MMAP requires NO system calls after initial setup!\n\n");
    
    /* Cleanup */
    printf("--- Cleanup ---\n");
    if (munmap(mapped_mem, map_size) == -1) {
        perror("munmap failed");
    } else {
        printf("munmap() succeeded\n");
    }
    
    close(fd);
    printf("\n=== All tests completed ===\n");
    
    return 0;
}

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd;
    char buffer[100];
    ssize_t bytes;
    
    printf("Opening device...\n");
    fd = open("/dev/mychardev", O_RDONLY);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }
    
    printf("Device opened\n");
    printf("Calling read() - will BLOCK until data arrives...\n");
    printf(">>> Press some keys to trigger interrupt and wake me up! <<<\n\n");
    
    // This will BLOCK here until interrupt occurs
    bytes = read(fd, buffer, sizeof(buffer));
    
    if (bytes < 0) {
        perror("Read failed");
    } else {
        printf("\n=== WOKE UP! ===\n");
        printf("Read %ld bytes from device\n", bytes);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            printf("Data: %s\n", buffer);
        }
    }
    
    close(fd);
    return 0;
}

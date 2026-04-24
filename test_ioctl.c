#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

/* Must match driver definitions */
#define IOCTL_MAGIC 'M'
#define BUFFER_SIZE 1024

struct ioctl_data {
    char buffer[BUFFER_SIZE];
    size_t size;
};

#define SET_BUFFER_VALUE _IOW(IOCTL_MAGIC, 1, struct ioctl_data)
#define GET_BUFFER_VALUE _IOR(IOCTL_MAGIC, 2, struct ioctl_data)

int main() {
    int fd;
    struct ioctl_data data;
    int ret;
    
    printf("=== IOCTL Test Program ===\n\n");
    
    /* Open device */
    fd = open("/dev/mychardev", O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }
    printf("Device opened successfully\n\n");
    
    /* Test 1: SET_BUFFER_VALUE */
    printf("--- Test 1: SET_BUFFER_VALUE ---\n");
    memset(&data, 0, sizeof(data));
    strcpy(data.buffer, "Hello from IOCTL! This is a test message.");
    data.size = strlen(data.buffer);
    
    printf("Sending %zu bytes via IOCTL: '%s'\n", data.size, data.buffer);
    
    ret = ioctl(fd, SET_BUFFER_VALUE, &data);
    if (ret < 0) {
        perror("SET_BUFFER_VALUE failed");
        close(fd);
        return 1;
    }
    printf("SET_BUFFER_VALUE succeeded!\n\n");
    
    /* Test 2: GET_BUFFER_VALUE */
    printf("--- Test 2: GET_BUFFER_VALUE ---\n");
    memset(&data, 0, sizeof(data));
    
    printf("Retrieving data via IOCTL...\n");
    
    ret = ioctl(fd, GET_BUFFER_VALUE, &data);
    if (ret < 0) {
        perror("GET_BUFFER_VALUE failed");
        close(fd);
        return 1;
    }
    
    printf("GET_BUFFER_VALUE succeeded!\n");
    printf("Retrieved %zu bytes: '%s'\n\n", data.size, data.buffer);
    
    /* Test 3: Compare read() with IOCTL */
    printf("--- Test 3: Verify with read() ---\n");
    char read_buf[BUFFER_SIZE] = {0};
    lseek(fd, 0, SEEK_SET);  /* Reset offset */
    ssize_t bytes_read = read(fd, read_buf, sizeof(read_buf));
    printf("read() returned %ld bytes: '%s'\n\n", bytes_read, read_buf);
    
    /* Test 4: Invalid IOCTL command */
    printf("--- Test 4: Invalid IOCTL (should fail) ---\n");
    ret = ioctl(fd, 0xDEADBEEF, NULL);
    if (ret < 0) {
        printf("Invalid IOCTL correctly rejected (expected)\n\n");
    }
    
    close(fd);
    printf("=== All tests completed ===\n");
    
    return 0;
}

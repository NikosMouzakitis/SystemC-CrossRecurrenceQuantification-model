// main_smart.c - Smart DMA handling
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#define DMA_OFFSET    0x10000
#define TRIGGER_REG   0x1000
#define TRIGGER_MAGIC 0xDEADBEEFDEADBEEFULL
#define N_SAMPLES     512

static int load_signal_from_file(const char *filename, double *signal, int max_samples) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error opening %s: %s\n", filename, strerror(errno));
        return -1;
    }
    
    int i = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && i < max_samples) {
        if (line[0] == '\n' || line[0] == '#') continue;
        char *endptr;
        double value = strtod(line, &endptr);
        if (endptr != line) signal[i++] = value;
    }
    fclose(fp);
    
    for (; i < max_samples; i++) signal[i] = 0.0;
    printf("Loaded %d samples from %s\n", i, filename);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *sig1_file = "systemc_input_F7_T7.txt";
    const char *sig2_file = "systemc_input_FP1_F7.txt";
    
    if (argc >= 3) {
        sig1_file = argv[1];
        sig2_file = argv[2];
    }
    
    double sig1[N_SAMPLES], sig2[N_SAMPLES];
    
    printf("Loading signals...\n");
    if (load_signal_from_file(sig1_file, sig1, N_SAMPLES) < 0 ||
        load_signal_from_file(sig2_file, sig2, N_SAMPLES) < 0) {
        return 1;
    }
    
    // Open device
    int fd = open("/dev/cpcidev_pci", O_RDWR);
    if (fd < 0) { 
        perror("open /dev/cpcidev_pci");
        return 1; 
    }
    
    // Map memory
    void *base = mmap(NULL, 2*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { 
        perror("mmap");
        close(fd);
        return 1; 
    }
    
    uint8_t *dma = (uint8_t*)base + DMA_OFFSET;
    volatile uint64_t *trigger = (volatile uint64_t*)((uint8_t*)base + TRIGGER_REG);
    
    // **KEY: Read current state and determine next ID**
    uint64_t current_id = *(uint64_t*)(dma + 16);
    printf("\nCurrent DMA state:\n");
    printf("  ID in DMA: %lu\n", current_id);
    printf("  R value: %f\n", *(double*)dma);
    printf("  Opcode: %u\n", *(uint32_t*)(dma + 8));
    
    // Determine what ID to use
    uint64_t use_id;
    if (current_id == 0) {
        // Fresh DMA - start with 1
        use_id = 1;
        printf("DMA appears fresh, starting with ID=1\n");
    } else {
        // DMA has previous state - use current ID
        use_id = current_id;
        printf("Using existing ID=%lu from DMA\n", use_id);
    }
    
    // Optional: Wait for any previous computation to complete
    printf("Checking if previous computation is in progress...\n");
    int check_timeout = 100; // 100ms
    while (*(uint64_t*)(dma + 16) != use_id && check_timeout-- > 0) {
        printf("  Waiting for ID to stabilize... (current: %lu, expected: %lu)\n", 
               *(uint64_t*)(dma + 16), use_id);
        usleep(1000);
    }
    
    // Write our parameters
    printf("\nSetting up computation:\n");
    printf("  R = 0.15\n");
    printf("  Opcode = 42\n");
    printf("  ID = %lu\n", use_id);
   


    uint64_t start = now_ns();

    *(double*)dma = 0.15;
    *(uint32_t*)(dma + 8) = 42;
    *(uint64_t*)(dma + 16) = use_id;
    
    // Copy signals
    memcpy(dma + 24, sig1, sizeof(sig1));
    memcpy(dma + 24 + sizeof(sig1), sig2, sizeof(sig2));
    
    // Memory barrier
    asm volatile("fence w,w" ::: "memory");
    // Trigger computation
    printf("\nSending trigger...\n");
    *trigger = TRIGGER_MAGIC;
    asm volatile("fence w,w" ::: "memory");
    
    // Wait for completion (ID should increment)
    //printf("Waiting for computation (ID should change from %lu)...\n", use_id);
    
    int timeout = 0;
    int max_timeout = 10000; // 10 seconds
    
    uint64_t start_id = use_id;
    /*
    while (*(uint64_t*)(dma + 16) == start_id) {
        usleep(1000);
        timeout++;
        
        // Progress indicator
        if (timeout % 1000 == 0) {
            printf("  Waiting... (%d ms)\n", timeout);
        }
        
        if (timeout > max_timeout) {
            printf("\nTIMEOUT: Computation not completed after %d ms\n", timeout);
            printf("Current ID: %lu (still same as start)\n", *(uint64_t*)(dma + 16));
            break;
        }
    }
    */ 
    // Check results
     struct pollfd pfd = {
	.fd = fd,
	.events = POLLIN
	};
     printf("waiting for CRQA completion\n");
    
     poll(&pfd, 1, -1);


    uint64_t final_id = *(uint64_t*)(dma + 16);
    if (final_id != start_id) {
	
	uint64_t end = now_ns();
	double elapsed_ms = (end - start) / 1e6;
	printf("CRQA cycle time = %.3f ms\n", elapsed_ms);

        double *res = (double*)(dma + 24 + 8192);
        printf("\n=== COMPUTATION COMPLETE ===\n");
        printf("ID changed: %lu -> %lu\n", start_id, final_id);
        printf("Execution time: %d ms\n", timeout);
        printf("\n=== CRQA RESULTS ===\n");
        printf("Epsilon = %.6f\n", res[0]);
        printf("RR      = %.6f\n", res[1]);
        printf("DET     = %.6f\n", res[2]);
        printf("L       = %.6f\n", res[3]);
        printf("L_max   = %.6f\n", res[4]);
        printf("DIV     = %.6f\n", res[5]);
        printf("ENTR    = %.6f\n", res[6]);
        printf("LAM     = %.6f\n", res[7]);
        
        // Save the final state for next run
        printf("\nNext run should use ID = %lu\n", final_id);
    } else {
        printf("\nCOMPUTATION FAILED\n");
        printf("ID unchanged: %lu\n", final_id);
        printf("Possible issues:\n");
        printf("  1. SystemC server not running\n");
        printf("  2. QEMU device not loaded\n");
        printf("  3. Socket connection failed\n");
        
        // Try to diagnose
        printf("\nDebug info:\n");
        printf("  Trigger register: 0x%lx\n", *trigger);
        printf("  DMA ID field: %lu\n", final_id);
    }
    
    // Cleanup
    munmap(base, 2*1024*1024);
    close(fd);
    
    return (final_id != start_id) ? 0 : 1;
}

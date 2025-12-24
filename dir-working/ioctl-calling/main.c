/* main.c - userspace program with proper reset between runs */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "crqa_ioctl.h"
#define DEVICE "/dev/cpcidev_pci"
#define SIG1_FILE "systemc_input_FP1_F7.txt"
#define SIG2_FILE "systemc_input_F7_T7.txt"

#define N_SAMPLES 512
#include <time.h>
#include <stdint.h>
#include <stdio.h>

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Function to load signal from file */
int load_signal_from_file(const char *filename, double *signal, int max_samples) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error opening file %s: %s\n", filename, strerror(errno));
        return -1;
    }

    int count = 0;
    char line[256];

    while (count < max_samples && fgets(line, sizeof(line), fp)) {
        // Skip empty lines and comments
        if (line[0] == '\n' || line[0] == '#')
            continue;

        double value;
        if (sscanf(line, "%lf", &value) == 1) {
            signal[count] = value;
            count++;
        }
    }

    fclose(fp);

    if (count < max_samples) {
        fprintf(stderr, "Warning: File %s only contains %d values (expected %d)\n",
                filename, count, max_samples);
        // Fill remaining with zeros
        for (int i = count; i < max_samples; i++) {
            signal[i] = 0.0;
        }
    }

    printf("Loaded %d samples from %s\n", count, filename);
    return count;
}

/* Function to print signal statistics */
void print_signal_stats(const char *name, double *signal, int n) {
    double min = signal[0];
    double max = signal[0];
    double sum = 0.0;

    for (int i = 0; i < n; i++) {
        if (signal[i] < min) min = signal[i];
        if (signal[i] > max) max = signal[i];
        sum += signal[i];
    }

    double mean = sum / n;
    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = signal[i] - mean;
        var += diff * diff;
    }
    var /= n;
    double stddev = sqrt(var);

    printf("%s: min=%.4f, max=%.4f, mean=%.4f, stddev=%.4f\n",
           name, min, max, mean, stddev);
}

/* Function to reset device state between runs */
void reset_device_state(int fd) {
    printf("Resetting device state...\n");

    // Set R to 0
    double zero_R = 0.0;
    ioctl(fd, IOCTL_SET_R, &zero_R);

    // Set all signal values to 0
    int zero_idx = 0;
    double zero_val = 0.0;

    for (int i = 0; i < N_SAMPLES; i++) {
        ioctl(fd, IOCTL_SET_SIG1_IDX, &zero_idx);
        ioctl(fd, IOCTL_SET_SIG1_VAL, &zero_val);

        ioctl(fd, IOCTL_SET_SIG2_IDX, &zero_idx);
        ioctl(fd, IOCTL_SET_SIG2_VAL, &zero_val);
    }

    // Set opcode to 0
    int zero_opcode = 0;
    ioctl(fd, IOCTL_SET_OPCODE, &zero_opcode);

    printf("Device reset complete\n");
}

int main(void) {
    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    double R = 0.15;
    int opcode = 42;
    double *sig1 = NULL;
    double *sig2 = NULL;
    uint32_t *sig1_idx = NULL;
    uint32_t *sig2_idx = NULL;

    /* Variables for all CRQA metrics */
    double epsilon = 0.0;
    double recurrence_rate = 0.0;
    double determinism = 0.0;
    double laminarity = 0.0;
    double trapping_time = 0.0;
    double max_diag_line = 0.0;
    double divergence = 0.0;
    double entropy = 0.0;

    int i, ret;
    double cpu_time_used;

    /* Allocate signals and index arrays */
    sig1 = malloc(N_SAMPLES * sizeof(double));
    sig2 = malloc(N_SAMPLES * sizeof(double));
    sig1_idx = malloc(N_SAMPLES * sizeof(uint32_t));
    sig2_idx = malloc(N_SAMPLES * sizeof(uint32_t));

    if (!sig1 || !sig2 || !sig1_idx || !sig2_idx) {
        fprintf(stderr, "malloc failed\n");
        goto cleanup;
    }

    printf("=== CRQA PCI Device Test ===\n");

    /* Reset device first to clear any previous state */
    reset_device_state(fd);

    /* Load signals from files */
    printf("Loading signals from files...\n");
    int loaded1 = load_signal_from_file(SIG1_FILE, sig1, N_SAMPLES);
    int loaded2 = load_signal_from_file(SIG2_FILE, sig2, N_SAMPLES);

    if (loaded1 < 0 || loaded2 < 0) {
        fprintf(stderr, "Failed to load signal files\n");
        goto cleanup;
    }

    /* Initialize index arrays */
    for (i = 0; i < N_SAMPLES; i++) {
        sig1_idx[i] = i;
        sig2_idx[i] = i;
    }

    /* Print signal statistics */
    print_signal_stats("Signal 1", sig1, N_SAMPLES);
    print_signal_stats("Signal 2", sig2, N_SAMPLES);

    /* Set R */
    printf("\nSetting R = %f\n", R);
    ret = ioctl(fd, IOCTL_SET_R, &R);
    if (ret < 0) {
        perror("IOCTL_SET_R");
        goto cleanup;
    }

    uint64_t start = now_ns();
    /* Fill SIG1 */
//    printf("Filling SIG1 array...\n");
    for (i = 0; i < N_SAMPLES; i++) {
        ret = ioctl(fd, IOCTL_SET_SIG1_IDX, &sig1_idx[i]);
        if (ret < 0) {
            perror("IOCTL_SET_SIG1_IDX");
            break;
        }

        ret = ioctl(fd, IOCTL_SET_SIG1_VAL, &sig1[i]);
        if (ret < 0) {
            perror("IOCTL_SET_SIG1_VAL");
            break;
        }
    }

    if (ret < 0) goto cleanup;
 //   printf("SIG1 array filled successfully.\n");

    /* Fill SIG2 */
  //  printf("Filling SIG2 array...\n");
    for (i = 0; i < N_SAMPLES; i++) {
        ret = ioctl(fd, IOCTL_SET_SIG2_IDX, &sig2_idx[i]);
        if (ret < 0) {
            perror("IOCTL_SET_SIG2_IDX");
            break;
        }

        ret = ioctl(fd, IOCTL_SET_SIG2_VAL, &sig2[i]);
        if (ret < 0) {
            perror("IOCTL_SET_SIG2_VAL");
            break;
        }
    }

    if (ret < 0) goto cleanup;
   // printf("SIG2 array filled successfully.\n");

    /* Set opcode - this signals that data is ready */
///    printf("Setting opcode = %d (data ready)\n", opcode);
    ret = ioctl(fd, IOCTL_SET_OPCODE, &opcode);
    if (ret < 0) {
        perror("IOCTL_SET_OPCODE");
        goto cleanup;
    }

  //  printf("\n=== Starting CRQA Computation ===\n");


    /* Read epsilon (triggers computation) */
//    printf("Triggering computation...\n");
    ret = ioctl(fd, IOCTL_GET_EPSILON, &epsilon);
    if (ret < 0) {
        perror("IOCTL_GET_EPSILON");
        goto cleanup;
    }

    /* Small delay to ensure computation completes */
// usleep(5000); // 100ms delay

    /* Read all other metrics */
    printf("Reading CRQA metrics...\n");
    ret = ioctl(fd, IOCTL_GET_RECURRENCE_RATE, &recurrence_rate);
    if (ret < 0) perror("IOCTL_GET_RECURRENCE_RATE");

    ret = ioctl(fd, IOCTL_GET_DETERMINISM, &determinism);
    if (ret < 0) perror("IOCTL_GET_DETERMINISM");

    ret = ioctl(fd, IOCTL_GET_LAMINARITY, &laminarity);
    if (ret < 0) perror("IOCTL_GET_LAMINARITY");

    ret = ioctl(fd, IOCTL_GET_TRAPPING_TIME, &trapping_time);
    if (ret < 0) perror("IOCTL_GET_TRAPPING_TIME");

    ret = ioctl(fd, IOCTL_GET_MAX_DIAG_LINE, &max_diag_line);
    if (ret < 0) perror("IOCTL_GET_MAX_DIAG_LINE");

    ret = ioctl(fd, IOCTL_GET_DIVERGENCE, &divergence);
    if (ret < 0) perror("IOCTL_GET_DIVERGENCE");

    ret = ioctl(fd, IOCTL_GET_ENTROPY, &entropy);
    if (ret < 0) perror("IOCTL_GET_ENTROPY");

    /* End timing */
    
	uint64_t end = now_ns();
	double elapsed_ms = (end - start) / 1e6;

	printf("CRQA cycle time = %.3f ms\n", elapsed_ms);
    /* Display results */
    printf("\n=== CRQA Results ===\n");
    printf("Configuration:\n");
    printf("  R = %.3f, N = %d samples\n", R, N_SAMPLES);

    printf("  Signal files: %s, %s\n", SIG1_FILE, SIG2_FILE);
    printf("\nMetrics:\n");
    printf("  Epsilon (DET):               %10.6f\n", epsilon);
    printf("  Recurrence Rate (RR):        %10.6f\n", recurrence_rate);
    printf("  Determinism (DET):           %10.6f\n", determinism);
    printf("  Laminarity (LAM):            %10.6f\n", laminarity);
    printf("  Trapping Time (TT):          %10.6f\n", trapping_time);
    printf("  Max Diagonal Line (MAXL):    %10.6f\n", max_diag_line);
    printf("  Divergence (DIV):            %10.6f\n", divergence);
    printf("  Entropy (ENTR):              %10.6f\n", entropy);
    printf("\nPerformance:\n");
    printf("  Total time: %.3f seconds\n", cpu_time_used);
    printf("============================\n");

cleanup:
    /* Cleanup */
    if (sig1) free(sig1);
    if (sig2) free(sig2);
    if (sig1_idx) free(sig1_idx);
    if (sig2_idx) free(sig2_idx);

    if (fd >= 0) close(fd);

    if (ret < 0) {
        return 1;
    }
    return 0;
}

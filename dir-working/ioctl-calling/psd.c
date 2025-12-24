/* psd.c - SIMPLIFIED VERSION */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qapi/visitor.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define SOCKET_PATH "/tmp/crqa_socket"
#define N_SAMPLES 512

#define TYPE_PCI_CUSTOM_DEVICE "crqadev"
typedef struct CpcidevState CpcidevState;

DECLARE_INSTANCE_CHECKER(CpcidevState, CPCIDEV, TYPE_PCI_CUSTOM_DEVICE)

struct CpcidevState {
    PCIDevice pdev;
    MemoryRegion mmio;

    uint32_t opcode;
    double R;
    double sig1[N_SAMPLES];
    double sig2[N_SAMPLES];
    
    /* All CRQA metrics received from SystemC */
    double epsilon;
    double recurrence_rate;
    double determinism;
    double laminarity;
    double trapping_time;
    double max_diag_line;
    double divergence;
    double entropy;

    /* temporary indexes for two-step protocol */
    uint32_t sig1_index;
    uint32_t sig2_index;
    
    /* State tracking */
    int data_ready;          // Flag indicating data is ready for computation
    int sig1_filled;         // Counter for sig1 filled
    int sig2_filled;         // Counter for sig2 filled

    int sockfd;
};

/* Response message structure matching SystemC */
struct sc_response {
    double epsilon;
    double recurrence_rate;
    double determinism;
    double laminarity;
    double trapping_time;
    double max_diag_line;
    double divergence;
    double entropy;
} __attribute__((packed));

/* Input message structure with data ready flag */
struct sc_msg {
    double R;
    double sig1[N_SAMPLES];
    double sig2[N_SAMPLES];
    int32_t opcode;
    int data_ready;
} __attribute__((packed));

static int connect_to_systemc(CpcidevState *s)
{
    // Always create new connection
    if (s->sockfd >= 0) {
        close(s->sockfd);
        s->sockfd = -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket()");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "CRQAPCI: Cannot connect to SystemC: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    
    s->sockfd = fd;
    printf("CRQAPCI: Connected to SystemC server (fd=%d).\n", s->sockfd);
    return 0;
}

static int request_crqa_from_systemc(CpcidevState *s)
{
    // Connect for each request
    if (connect_to_systemc(s) < 0) {
        fprintf(stderr, "CRQAPCI: Cannot connect to SystemC\n");
        return -1;
    }

    struct sc_msg msg;
    ssize_t rt;

    msg.R = s->R;
    memcpy(msg.sig1, s->sig1, sizeof(msg.sig1));
    memcpy(msg.sig2, s->sig2, sizeof(msg.sig2));
    msg.opcode = s->opcode;
    msg.data_ready = s->data_ready;

//    printf("CRQAPCI: Sending data to SystemC (R=%f, data_ready=%d)\n", s->R, s->data_ready);

    /* write the message */
    rt = write(s->sockfd, &msg, sizeof(msg));
    if (rt != (ssize_t)sizeof(msg)) {
        fprintf(stderr, "CRQAPCI: write() to SystemC failed (wrote %zd of %zu): %s\n",
                rt, sizeof(msg), strerror(errno));
        close(s->sockfd);
        s->sockfd = -1;
        return -1;
    }

    /* read the complete response with all CRQA metrics */
    struct sc_response resp;
    rt = read(s->sockfd, &resp, sizeof(resp));
    if (rt != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "CRQAPCI: read() from SystemC failed (read %zd of %zu): %s\n",
                rt, sizeof(resp), strerror(errno));
        close(s->sockfd);
        s->sockfd = -1;
        return -1;
    }

    /* Store all metrics */
    s->epsilon = resp.epsilon;
    s->recurrence_rate = resp.recurrence_rate;
    s->determinism = resp.determinism;
    s->laminarity = resp.laminarity;
    s->trapping_time = resp.trapping_time;
    s->max_diag_line = resp.max_diag_line;
    s->divergence = resp.divergence;
    s->entropy = resp.entropy;

//    printf("CRQAPCI: Received response from SystemC:\n");
//    printf("  epsilon (DET):         %f\n", s->epsilon);
//    printf("  recurrence_rate (RR):  %f\n", s->recurrence_rate);

    // Close connection after receiving response
    close(s->sockfd);
    s->sockfd = -1;

    return 0;
}

static void check_data_ready(CpcidevState *s)
{
    // Simple check: data is ready when opcode is set and both arrays have non-zero data
    s->data_ready = (s->opcode != 0 && s->sig1_filled && s->sig2_filled);
/*    if (s->data_ready) {
        printf("CRQAPCI: Data ready for computation\n");
    }
*/
}

static uint64_t cpcidev_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CpcidevState *s = opaque;
    uint64_t val = 0;

//    fprintf(stderr, "CRQAPCI: mmio_read addr=0x%llx size=%u\n",(unsigned long long)addr, size);

    switch (addr) {
        /* Device ID read for verification */
        case 0x00:
            return 0x11223344ULL;

        /* Trigger computation and read epsilon (backward compatibility) */
        case 0x40: {
 //           printf("CRQAPCI: Triggering computation...\n");
            if (request_crqa_from_systemc(s) == 0) {
                memcpy(&val, &s->epsilon, sizeof(double));
  //              printf("CRQAPCI: Computation successful, epsilon=%f\n", s->epsilon);
            } else {
                val = 0;
                printf("CRQAPCI: Computation failed\n");
            }
            return val;
        }

        /* Read all CRQA metrics at specific addresses */
        case 0x48:  /* recurrence_rate */
            memcpy(&val, &s->recurrence_rate, sizeof(double));
            break;
            
        case 0x50:  /* determinism */
            memcpy(&val, &s->determinism, sizeof(double));
            break;
            
        case 0x58:  /* laminarity */
            memcpy(&val, &s->laminarity, sizeof(double));
            break;
            
        case 0x60:  /* trapping_time */
            memcpy(&val, &s->trapping_time, sizeof(double));
            break;
            
        case 0x68:  /* max_diag_line */
            memcpy(&val, &s->max_diag_line, sizeof(double));
            break;
            
        case 0x70:  /* divergence */
            memcpy(&val, &s->divergence, sizeof(double));
            break;
            
        case 0x78:  /* entropy */
            memcpy(&val, &s->entropy, sizeof(double));
            break;

        default:
            val = 0;
            break;
    }
    
    return val;
}

static void cpcidev_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CpcidevState *s = opaque;
    
//    fprintf(stderr, "QEMU receives::: CRQAPCI: mmio_write addr=0x%llx raw=0x%016llx size=%u\n", (unsigned long long)addr, (unsigned long long)val, size);

    switch (addr) {
        case 0x08: { /* R (aligned 8-byte slot) */
            uint64_t raw = 0;
            if (size == 8) {
                raw = (uint64_t)val;
            } else {
                raw = (uint64_t)val;
            }
            double d;
            memcpy(&d, &raw, sizeof(double));
            s->R = d;
 //           printf("  -> R set = %f (raw=0x%016llx)\n", s->R, (unsigned long long)raw);
            break;
        }

        case 0x18: { /* sig1_index */
            uint32_t idx = (uint32_t)val;
            if (idx < N_SAMPLES) {
                s->sig1_index = idx;
            } else {
                printf("  -> sig1_index out of range %u\n", idx);
            }
            break;
        }

        case 0x20: { /* sig1_value */
            double d;
            if (size == 8) {
                memcpy(&d, &val, sizeof(double));
            } else {
                uint64_t tmp = 0;
                memcpy(&tmp, &val, size);
                memcpy(&d, &tmp, sizeof(double));
            }
            if (s->sig1_index < N_SAMPLES && isfinite(d)) {
                s->sig1[s->sig1_index] = d;
                
                // Mark sig1 as filled when we write to index 511
                if (s->sig1_index == 511) {
                    s->sig1_filled = 1;
//                    printf("  -> SIG1 array filled (512 values)\n");
                }
            }
            break;
        }

        case 0x28: { /* sig2_index */
            uint32_t idx = (uint32_t)val;
            if (idx < N_SAMPLES) {
                s->sig2_index = idx;
            } else {
                printf("  -> sig2_index out of range %u\n", idx);
            }
            break;
        }

        case 0x30: { /* sig2_value */
            double d;
            if (size == 8) {
                memcpy(&d, &val, sizeof(double));
            } else {
                uint64_t tmp = 0;
                memcpy(&tmp, &val, size);
                memcpy(&d, &tmp, sizeof(double));
            }
            if (s->sig2_index < N_SAMPLES && isfinite(d)) {
                s->sig2[s->sig2_index] = d;
                
                // Mark sig2 as filled when we write to index 511
                if (s->sig2_index == 511) {
                    s->sig2_filled = 1;
 //                   printf("  -> SIG2 array filled (512 values)\n");
                }
            }
            break;
        }

        case 0x38: { /* opcode */
            s->opcode = (uint32_t)val;
//            printf("  -> opcode = %u\n", s->opcode);
            
            // When opcode is set, check if data is ready
            check_data_ready(s);
            break;
        }

        default:
            break;
    }
}

static const MemoryRegionOps cpcidev_mmio_ops = {
    .read = cpcidev_mmio_read,
    .write = cpcidev_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void pci_cpcidev_realize(PCIDevice *pdev, Error **errp)
{
    CpcidevState *s = CPCIDEV(pdev);

    uint8_t *pci_conf = pdev->config;
//    printf("CRQAPCI_REALIZE: pid=%d\n", (int)getpid());

    pci_config_set_interrupt_pin(pci_conf, 1);
    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    /* Initialize */
    s->sockfd = -1;
    memset(s->sig1, 0, sizeof(s->sig1));
    memset(s->sig2, 0, sizeof(s->sig2));
    s->sig1_index = 0;
    s->sig2_index = 0;
    s->R = 0.0;
    s->opcode = 0;
    s->data_ready = 0;
    s->sig1_filled = 0;
    s->sig2_filled = 0;
    
    /* Initialize CRQA metrics to zero */
    s->epsilon = 0.0;
    s->recurrence_rate = 0.0;
    s->determinism = 0.0;
    s->laminarity = 0.0;
    s->trapping_time = 0.0;
    s->max_diag_line = 0.0;
    s->divergence = 0.0;
    s->entropy = 0.0;

    memory_region_init_io(&s->mmio, OBJECT(s), &cpcidev_mmio_ops, s,
                          "crqa-mmio", 2 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void pci_cpcidev_uninit(PCIDevice *pdev)
{
    CpcidevState *s = CPCIDEV(pdev);
    if (s->sockfd >= 0) {
        close(s->sockfd);
    }
}

static void cpcidev_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);
    
    k->realize = pci_cpcidev_realize;
    k->exit = pci_cpcidev_uninit;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = 0xdada;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void pci_custom_device_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    };
    static const TypeInfo custom_pci_device_info = {
        .name          = TYPE_PCI_CUSTOM_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(CpcidevState),
        .class_init    = cpcidev_class_init,
        .interfaces = interfaces,
    };
    type_register_static(&custom_pci_device_info);
}

type_init(pci_custom_device_register_types)

/* psd.c - QEMU PCI Device - CRQA Accelerator */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/irq.h"
#include "qom/object.h"
#include "qemu/module.h"
#include "hw/riscv/msi_harts.h"
#include "hw/intc/riscv_imsic.h" 
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#define SOCKET_PATH      "/tmp/crqa_socket"
#define N_SAMPLES        512
#define BUFFER_OFFSET    0x10000        /* shared buffer starts at 64 KB */
#define BUFFER_SIZE      (16 * 1024)    /* 16 KB shared buffer */
#define TRIGGER_REG      0x1000
#define TRIGGER_MAGIC    0xDEADBEEFDEADBEEFULL

#define TYPE_PCI_CRQADEV "crqa-pci-dev"

typedef struct CrqaDevState CrqaDevState;
DECLARE_INSTANCE_CHECKER(CrqaDevState, CRQADEV, TYPE_PCI_CRQADEV)

struct CrqaDevState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion buffer_mr;     /* renamed from dma_mr */
    uint8_t *buffer;            /* renamed from dma_buf */
    uint64_t trigger_counter;
    int sockfd;                 /* persistent socket */
    int eventfd;		/* used for the notification mechanism (SystemC--> QEMU) */
  
    QEMUBH *irq_bh;
    bool pending_irq; 

    double   R;
    uint32_t opcode;
    double   sig1[N_SAMPLES];
    double   sig2[N_SAMPLES];
    double   results[8];
};

static void crqa_irq_bh(void *opaque)
{
    CrqaDevState *s = opaque;
    PCIDevice *pdev = PCI_DEVICE(s);
    
    if (!s->pending_irq) {
        return;
    }

    s->pending_irq = false;

    if (!msi_enabled(pdev)) {
        /* case where driver not ready yet */
        return;
    }

    //testing
    /*
     MSIMessage msg = msi_get_message(pdev, 0);
     printf("CRQA_DEV: MSI message - address: 0x%"PRIx64", data: 0x%x\n", msg.address, msg.data);
        // Check if address is in IMSIC range
     if ((msg.address & 0xffffffff) != 0x28000000) {
        printf("CRQA_DEV: WARNING: MSI address 0x%"PRIx64" not in IMSIC range!\n", msg.address);
     }
    */ 
    //copy results and do MSI.
    // Check what results SystemC sent
    printf("SystemC results stored in s->results:\n");
    for (int i = 0; i < 8; i++) {
        printf("  results[%d] = %f\n", i, s->results[i]);
    }


    uint8_t *buf = s->buffer;
    double *res = (double *)(buf + 24 + 8192);

    memcpy(res, s->results, sizeof(s->results));
//    s->trigger_counter++;
//    *(uint64_t *)(buf + 16) = s->trigger_counter;
    
    // Check what's in the buffer AFTER copying
    printf("Buffer results AFTER copy:\n");
    for (int i = 0; i < 8; i++) {
        printf("  buffer[%d] = %f\n", i, res[i]);
    }
    

    printf("=== CRQA IRQ BH ===\n");

    if (msi_enabled(pdev)) {
        MSIMessage msg = msi_get_message(pdev, 0);

        printf("Current MSI config:\n");
        printf("  Address: 0x%"PRIx64"\n", msg.address);
        printf("  Data: 0x%x\n", msg.data);

	stl_le_phys(&address_space_memory,msg.address, msg.data);
/*
        // Test: Try MSI to hart 0
        printf("\nTesting MSI to hart 0 (address 0x28000000):\n");
        MSIMessage test_msg_0;
        test_msg_0.address = 0x28000000;
        test_msg_0.data = msg.data;  // Your vector

        printf("  Writing to 0x28000000...\n");
        stl_le_phys(&address_space_memory, test_msg_0.address, test_msg_0.data);

        // Test: Try MSI to hart 1
        printf("\nTesting MSI to hart 1 (address 0x28001000):\n");
        MSIMessage test_msg_1;
        test_msg_1.address = 0x28001000;
        test_msg_1.data = msg.data;

        printf("  Writing to 0x28001000...\n");
        stl_le_phys(&address_space_memory, test_msg_1.address, test_msg_1.data);

        // Also try the configured address
//        printf("\nTrying configured address:\n");
 //       msi_notify(pdev, 0);
*/
    }

   
    //MSI notify
/*     
    if (msi_enabled(pdev)) {
	printf("CRQA_DEV: calling msi_notify()\n");
        msi_notify(pdev, 0);
    }
*/    
    //working for SMP 1 triggers an MSI observable in linux kernel driver.
    //printf("test\n");
    //riscv_imsic_set_pending(msi_harts[0], 5);
}

static int send_eventfd(int sock, int eventfd)
{
    struct msghdr msg = {};
    char buf[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    char dummy = 'E';

    iov.iov_base = &dummy;
    iov.iov_len  = sizeof(dummy);

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));

    memcpy(CMSG_DATA(cmsg), &eventfd, sizeof(int));

    if (sendmsg(sock, &msg, 0) < 0) {
        perror("sendmsg(eventfd)");
        return -1;
    }

    printf("QEMU: sent eventfd %d to SystemC\n", eventfd);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────── */
static int connect_to_systemc(CrqaDevState *s)
{
    // if socket is already created, we execute here.
    if (s->sockfd >= 0) {
	printf("sockfd: %d\n",s->sockfd);
        int error = 0;
        socklen_t len = sizeof(error);
        int ret = getsockopt(s->sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
        if (ret == 0 && error == 0)
	{
	    printf("QEMU crqa-dev: sockfd already alive\n");
            return 0;
	}
        close(s->sockfd);
        s->sockfd = -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("CRQAPCI: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    /*making non-blocking the socket.*/
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    printf("CRQAPCI: Connecting to SystemC at %s...\n", SOCKET_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("CRQAPCI: connect() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    printf("CRQAPCI: Connected to SystemC (fd=%d)\n", fd);

    
    /* SEND EVENTFD ONCE */
    send_eventfd(fd, s->eventfd);


    s->sockfd = fd;
    return 0;
}

static int request_crqa(CrqaDevState *s)
{
    if (connect_to_systemc(s) < 0) {
        printf("CRQAPCI: Failed to connect to SystemC\n");
        return -1;
    }

    struct {
        double   R;
        double   sig1[N_SAMPLES];
        double   sig2[N_SAMPLES];
        int32_t  opcode;
        int32_t  ready;
    } __attribute__((packed)) msg = {
        .R = s->R,
        .opcode = s->opcode,
        .ready = 1
    };
    memcpy(msg.sig1, s->sig1, sizeof(msg.sig1));
    memcpy(msg.sig2, s->sig2, sizeof(msg.sig2));

    ssize_t n = write(s->sockfd, &msg, sizeof(msg));
    if (n != sizeof(msg)) {
        printf("CRQAPCI: Write failed (%zd bytes): %s\n", n, strerror(errno));
        close(s->sockfd);
        s->sockfd = -1;
        return -1;
    }

    /*
    n = read(s->sockfd, &s->results, sizeof(s->results));
    if (n != sizeof(s->results)) {
        printf("CRQAPCI: Read failed (%zd bytes): %s\n", n, strerror(errno));
        close(s->sockfd);
        s->sockfd = -1;
        return -1;
    }
   */
    //printf("CRQAPCI: Got results: epsilon=%.6f, RR=%.6f, DET=%.6f, L=%.6f\n",
     //      s->results[0], s->results[1], s->results[2], s->results[3]);

    return 0;   /* socket intentionally left open */
}

/* ────────────────────────────────────────────────────────────────────── */
static uint64_t crqa_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CrqaDevState *s = opaque;

    if (addr >= BUFFER_OFFSET && addr < BUFFER_OFFSET + BUFFER_SIZE) {
        uint8_t *ptr = s->buffer + (addr - BUFFER_OFFSET);
        switch (size) {
            case 1: return *(uint8_t *)ptr;
            case 2: return *(uint16_t *)ptr;
            case 4: return *(uint32_t *)ptr;
            case 8: return *(uint64_t *)ptr;
        }
    }
    return 0;
}

static void crqa_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CrqaDevState *s = opaque;
    //printf("CRQAPCI dev: mmio write *************** %lx\n", addr);
    if (addr == TRIGGER_REG && size == 8 && val == TRIGGER_MAGIC) {
        uint8_t *buf = s->buffer;
        double   *R      = (double   *)buf;
        uint32_t *opcode = (uint32_t *)(buf + 8);
        uint64_t *id     = (uint64_t *)(buf + 16);
        double   *sig1   = (double   *)(buf + 24);
        double   *sig2   = (double   *)(buf + 24 + 4096);
        double   *res    = (double   *)(buf + 24 + 8192);

        if (*id == s->trigger_counter) {
            //printf("CRQAPCI: Trigger received – running CRQA (R=%.2f, opcode=%u)\n", *R, *opcode);
            s->R = *R;
            s->opcode = *opcode;
            memcpy(s->sig1, sig1, sizeof(s->sig1));
            memcpy(s->sig2, sig2, sizeof(s->sig2));

            int retries = 3;
            while (retries-- > 0) {
                if (request_crqa(s) == 0) {
                    memcpy(res, s->results, sizeof(s->results));
                    s->trigger_counter++;
                    *id = s->trigger_counter;
                    //printf("CRQAPCI: CRQA completed successfully\n");
                    return;
                }
                printf("CRQAPCI: Retry %d/3\n", 3 - retries);
                usleep(100000);
            }
            printf("CRQAPCI: CRQA failed after retries\n");
            s->trigger_counter++;
            *id = s->trigger_counter;
        } else {
            printf("CRQAPCI: Ignoring trigger - ID mismatch (expected %lu, got %lu)\n",
                   s->trigger_counter, *id);
        }
        return;
    }

    if (addr >= BUFFER_OFFSET && addr < BUFFER_OFFSET + BUFFER_SIZE) {
        uint8_t *ptr = s->buffer + (addr - BUFFER_OFFSET);
        switch (size) {
            case 1: *(uint8_t *)ptr   = val; break;
            case 2: *(uint16_t *)ptr  = val; break;
            case 4: *(uint32_t *)ptr  = val; break;
            case 8: *(uint64_t *)ptr  = val; break;
        }
    }
}

static const MemoryRegionOps mmio_ops = {
    .read = crqa_mmio_read,
    .write = crqa_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,

    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },

  //  .valid.accepts = NULL,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};


static void crqa_event_handler(void *opaque)
{
	CrqaDevState *s = opaque;
	uint64_t val;

	if(read(s->eventfd, &val, sizeof(val)) < 0)
	{
		return;
	}
	//read results from the socket
	// Read results from socket
	ssize_t n = read(s->sockfd, &s->results, sizeof(s->results));
	if (n != sizeof(s->results)) {
		printf("CRQAPCI: async read failed (%zd bytes): %s\n", n, strerror(errno));
		return;
	}
	//update that we have pending irq
	//from SystemC completion.
	s->pending_irq = true;
	//schedule the interrupt delivery.
	qemu_bh_schedule(s->irq_bh);
        /*	
	// Copy results back to shared buffer
        uint8_t *buf = s->buffer;
	double *res = (double *)(buf + 24 + 8192);
	memcpy(res, s->results, sizeof(s->results));
	s->trigger_counter++;
	*(uint64_t *)(buf + 16) = s->trigger_counter; // update ID
	printf("QEMU: crqa-dev: async results received, IRQ pulse!\n");	
	//raise & clear (pulse alike)
	printf("QEMU: crqa-dev: igniting pulse\n");

	//qemu_irq_pulse(s->irq);
	PCIDevice *pdev = PCI_DEVICE(s);   // get PCIDevice pointer
	msi_notify(pdev, 0);   // send vector 0
	*/
}

/* ────────────────────────────────────────────────────────────────────── */
static void crqa_realize(PCIDevice *pdev, Error **errp)
{
    CrqaDevState *s = CRQADEV(pdev);
    DeviceState *dev = DEVICE(pdev);
    uint8_t *pci_conf = pdev->config;


    pci_config_set_interrupt_pin(pci_conf, 0);


      /* eventfd creation */
    s->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(s->eventfd <0)
    {
	    printf("QEMU: error eventfd: %d\n",s->eventfd);
	    exit(1);
    }
    printf("QEMU: crqa-dev eventfd: %d\n",s->eventfd);

    /*handler registration now */
    qemu_set_fd_handler(s->eventfd, crqa_event_handler, NULL, s);

    /* rest of PCI related realization */
    memory_region_init_io(&s->mmio, OBJECT(dev), &mmio_ops, s,
                          "crqa", 2 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
	

    // Enable MSI for the device
    int ret;
    Error *err=NULL;
    ret = msi_init(PCI_DEVICE(s), 0, 1, true, false, &err);
    if (ret < 0) {
        fprintf(stderr, "CRQAPCI: MSI init failed\n");
    } else {
	printf("CRQAPCI: MSI enabled\n");
    }
    s->buffer = g_malloc0(BUFFER_SIZE);
    memory_region_init_ram_ptr(&s->buffer_mr, OBJECT(dev), "crqa-buffer",
                               BUFFER_SIZE, s->buffer);
    memory_region_add_subregion(&s->mmio, BUFFER_OFFSET, &s->buffer_mr);

    s->trigger_counter = 1;
    s->sockfd = -1;

    //initialization of related stuff for the MSI delivery.
    s->pending_irq = false; 
    s->irq_bh = qemu_bh_new(crqa_irq_bh, s);


    printf("CRQAPCI: Device initialized – shared buffer at 0x%x (16 KB)\n", BUFFER_OFFSET);
    printf("CRQAPCI: Socket will be kept open between requests\n");

}

static void crqa_exit(PCIDevice *pdev)
{
    CrqaDevState *s = CRQADEV(pdev);

    if (s->sockfd >= 0) {
        printf("CRQAPCI: Closing SystemC connection (fd=%d)\n", s->sockfd);
        close(s->sockfd);
    }
    g_free(s->buffer);

    if(s->irq_bh){
	    qemu_bh_delete(s->irq_bh);
    }
}

static void crqa_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize   = crqa_realize;
    k->exit      = crqa_exit;
    k->vendor_id = 0x1234;
    k->device_id = 0xdada;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_OTHERS;

    dc->desc = "CRQA PCI Device (Persistent Connection)";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void crqa_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    };

    static const TypeInfo crqa_info = {
        .name          = TYPE_PCI_CRQADEV,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(CrqaDevState),
        .class_init    = crqa_class_init,
        .interfaces    = interfaces,
    };

    type_register_static(&crqa_info);
}

type_init(crqa_register_types)

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include "crqa_ioctl.h" 
#define BAR0 0
#define CDEV_NAME "cpcidev_pci"
#define QEMU_VENDOR_ID 0x1234
#define QEMU_DEVICE_ID 0xdada

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nikos Mouzakitis");
MODULE_DESCRIPTION("Linux driver for QEMU CRQA PCI device");

static struct pci_device_id pci_ids[] = {
    { PCI_DEVICE(QEMU_VENDOR_ID, QEMU_DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, pci_ids);

static struct pci_dev *pdev_global;
static void __iomem *mmio_base;
static int major;
static struct class *cpcidev_class;
static struct device *cpcidev_device;

static long cpcidev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int idx;
    double value;
    u64 raw;
    void __user *uarg = (void __user *)arg;

    switch (cmd) {
        case IOCTL_SET_R:
            /* copy double from user and write to offset 0x08 */
            if (copy_from_user(&value, uarg, sizeof(double)))
                return -EFAULT;
            memcpy(&raw, &value, sizeof(double));
            iowrite64(raw, mmio_base + 0x08);
//            pr_info("cpcidev: IOCTL_SET_R value=%f raw=0x%016llx\n",  value, (unsigned long long)raw);
            break;

        case IOCTL_SET_SIG1_IDX:
            if (copy_from_user(&idx, uarg, sizeof(int)))
                return -EFAULT;
            iowrite32(idx, mmio_base + 0x18);
//            pr_info("cpcidev: IOCTL_SET_SIG1_IDX idx=%d\n", idx);
            break;

        case IOCTL_SET_SIG1_VAL:
            if (copy_from_user(&value, uarg, sizeof(double)))
                return -EFAULT;
            memcpy(&raw, &value, sizeof(double));
            iowrite64(raw, mmio_base + 0x20);
 //           pr_info("cpcidev: IOCTL_SET_SIG1_VAL value=%f\n", value);
            break;

        case IOCTL_SET_SIG2_IDX:
            if (copy_from_user(&idx, uarg, sizeof(int)))
                return -EFAULT;
            iowrite32(idx, mmio_base + 0x28);
//            pr_info("cpcidev: IOCTL_SET_SIG2_IDX idx=%d\n", idx);
            break;

        case IOCTL_SET_SIG2_VAL:
            if (copy_from_user(&value, uarg, sizeof(double)))
                return -EFAULT;
            memcpy(&raw, &value, sizeof(double));
            iowrite64(raw, mmio_base + 0x30);
 //           pr_info("cpcidev: IOCTL_SET_SIG2_VAL value=%f\n", value);
            break;

        case IOCTL_SET_OPCODE:
            if (copy_from_user(&idx, uarg, sizeof(int)))
                return -EFAULT;
            iowrite32(idx, mmio_base + 0x38);
//            pr_info("cpcidev: IOCTL_SET_OPCODE opcode=%d\n", idx);
            break;

        case IOCTL_GET_EPSILON:
            /* Read from 0x40 triggers computation and returns epsilon */
            raw = ioread64(mmio_base + 0x40);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
//            pr_info("cpcidev: IOCTL_GET_EPSILON value=%f\n", value);
            break;

        case IOCTL_GET_RECURRENCE_RATE:
            raw = ioread64(mmio_base + 0x48);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
//            pr_info("cpcidev: IOCTL_GET_RECURRENCE_RATE value=%f\n", value);
            break;

        case IOCTL_GET_DETERMINISM:
            raw = ioread64(mmio_base + 0x50);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
//            pr_info("cpcidev: IOCTL_GET_DETERMINISM value=%f\n", value);
            break;

        case IOCTL_GET_LAMINARITY:
            raw = ioread64(mmio_base + 0x58);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
//            pr_info("cpcidev: IOCTL_GET_LAMINARITY value=%f\n", value);
            break;

        case IOCTL_GET_TRAPPING_TIME:
            raw = ioread64(mmio_base + 0x60);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
 //           pr_info("cpcidev: IOCTL_GET_TRAPPING_TIME value=%f\n", value);
            break;

        case IOCTL_GET_MAX_DIAG_LINE:
            raw = ioread64(mmio_base + 0x68);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
//            pr_info("cpcidev: IOCTL_GET_MAX_DIAG_LINE value=%f\n", value);
            break;

        case IOCTL_GET_DIVERGENCE:
            raw = ioread64(mmio_base + 0x70);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
//            pr_info("cpcidev: IOCTL_GET_DIVERGENCE value=%f\n", value);
            break;

        case IOCTL_GET_ENTROPY:
            raw = ioread64(mmio_base + 0x78);
            memcpy(&value, &raw, sizeof(double));
            if (copy_to_user((double __user *)uarg, &value, sizeof(double)))
                return -EFAULT;
//            pr_info("cpcidev: IOCTL_GET_ENTROPY value=%f\n", value);
            break;

        default:
            pr_info("cpcidev: Unknown ioctl command: 0x%x\n", cmd);
            return -EINVAL;
    }

    return 0;
}

static ssize_t cpcidev_read(struct file *filp, char __user *buf,
                            size_t len, loff_t *off)
{
    return -EINVAL; // not used
}

static ssize_t cpcidev_write(struct file *filp, const char __user *buf,
                             size_t len, loff_t *off)
{
    return -EINVAL; // not used
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = cpcidev_ioctl,
    .read  = cpcidev_read,
    .write = cpcidev_write,
};

static int cpcidev_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    int ret;
    resource_size_t mmio_start, mmio_len;

    ret = pci_enable_device(pdev);
    if (ret) {
        dev_err(&pdev->dev, "pci_enable_device failed\n");
        return ret;
    }

    ret = pci_request_region(pdev, BAR0, "cpcidev_mmio");
    if (ret) {
        dev_err(&pdev->dev, "pci_request_region failed\n");
        return ret;
    }

    mmio_start = pci_resource_start(pdev, BAR0);
    mmio_len   = pci_resource_len(pdev, BAR0);
    mmio_base  = pci_iomap(pdev, BAR0, mmio_len);
    if (!mmio_base) {
        dev_err(&pdev->dev, "pci_iomap failed\n");
        pci_release_region(pdev, BAR0);
        return -ENOMEM;
    }

    pdev_global = pdev;
    major = register_chrdev(0, CDEV_NAME, &fops);
    if (major < 0) {
        dev_err(&pdev->dev, "register_chrdev failed\n");
        pci_iounmap(pdev, mmio_base);
        pci_release_region(pdev, BAR0);
        return major;
    }
    
    // create /dev node
    cpcidev_class = class_create(CDEV_NAME);
    if (IS_ERR(cpcidev_class)) {
        unregister_chrdev(major, CDEV_NAME);
        pci_iounmap(pdev, mmio_base);
        pci_release_region(pdev, BAR0);
        return PTR_ERR(cpcidev_class);
    }

    cpcidev_device = device_create(cpcidev_class, &pdev->dev,
                       MKDEV(major, 0), NULL, CDEV_NAME);
    if (IS_ERR(cpcidev_device)) {
        class_destroy(cpcidev_class);
        unregister_chrdev(major, CDEV_NAME);
        pci_iounmap(pdev, mmio_base);
        pci_release_region(pdev, BAR0);
        return PTR_ERR(cpcidev_device);
    }

    dev_info(&pdev->dev, "cpcidev_pci registered major=%d\n", major);

    return 0;
}

static void cpcidev_remove(struct pci_dev *pdev)
{
    device_destroy(cpcidev_class, MKDEV(major, 0));
    class_destroy(cpcidev_class);
    unregister_chrdev(major, CDEV_NAME);
    pci_iounmap(pdev, mmio_base);
    pci_release_region(pdev, BAR0);
}

static struct pci_driver cpcidev_driver = {
    .name     = "cpcidev_driver",
    .id_table = pci_ids,
    .probe    = cpcidev_probe,
    .remove   = cpcidev_remove,
};

static int __init cpcidev_init(void)
{
    return pci_register_driver(&cpcidev_driver);
}

static void __exit cpcidev_exit(void)
{
    pci_unregister_driver(&cpcidev_driver);
}

module_init(cpcidev_init);
module_exit(cpcidev_exit);

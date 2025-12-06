#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/device.h>

#define CDEV_NAME "cpcidev_pci"
#define QEMU_VENDOR_ID 0x1234
#define QEMU_DEVICE_ID 0xdada

static struct pci_dev *pdev_global;
static dev_t dev_num;
static struct class *dev_class;
static struct device *dev_device;
static struct cdev cdev;

static int crqa_mmap(struct file *filp, struct vm_area_struct *vma)
{
    resource_size_t start = pci_resource_start(pdev_global, 0);
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (offset + size > 2*1024*1024) return -EINVAL;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    return remap_pfn_range(vma, vma->vm_start, (start + offset) >> PAGE_SHIFT, size, vma->vm_page_prot);
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .mmap  = crqa_mmap,
};

static int crqa_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret;

    ret = pci_enable_device(pdev);
    if (ret) return ret;

    ret = pci_request_region(pdev, 0, "crqa");
    if (ret) goto err_disable;

    pdev_global = pdev;

    dev_class = class_create("crqa");
    if (IS_ERR(dev_class)) { ret = PTR_ERR(dev_class); goto err_region; }

    ret = alloc_chrdev_region(&dev_num, 0, 1, CDEV_NAME);
    if (ret) goto err_class;

    cdev_init(&cdev, &fops);
    ret = cdev_add(&cdev, dev_num, 1);
    if (ret) goto err_chrdev;

    dev_device = device_create(dev_class, &pdev->dev, dev_num, NULL, CDEV_NAME);
    if (IS_ERR(dev_device)) { ret = PTR_ERR(dev_device); goto err_cdev; }

    dev_info(&pdev->dev, "CRQA zero-copy device ready\n");
    return 0;

err_cdev:
    cdev_del(&cdev);
err_chrdev:
    unregister_chrdev_region(dev_num, 1);
err_class:
    class_destroy(dev_class);
err_region:
    pci_release_region(pdev, 0);
err_disable:
    pci_disable_device(pdev);
    return ret;
}

static void crqa_remove(struct pci_dev *pdev)
{
    device_destroy(dev_class, dev_num);
    cdev_del(&cdev);
    unregister_chrdev_region(dev_num, 1);
    class_destroy(dev_class);
    pci_release_region(pdev, 0);
    pci_disable_device(pdev);
}

static const struct pci_device_id crqa_ids[] = {
    { PCI_DEVICE(QEMU_VENDOR_ID, QEMU_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, crqa_ids);

static struct pci_driver crqa_driver = {
    .name     = "crqa_pci",
    .id_table = crqa_ids,
    .probe    = crqa_probe,
    .remove   = crqa_remove,
};

module_pci_driver(crqa_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Zero-copy CRQA PCI device - single BAR");

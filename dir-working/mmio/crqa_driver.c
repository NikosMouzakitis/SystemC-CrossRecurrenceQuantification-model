#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/poll.h>
#include <linux/wait.h>

#define CDEV_NAME "cpcidev_pci"
#define QEMU_VENDOR_ID 0x1234
#define QEMU_DEVICE_ID 0xdada

static DECLARE_WAIT_QUEUE_HEAD(crqa_waitqueue);
static atomic_t data_ready = ATOMIC_INIT(0);


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

	//set flag that data are not-ready. new mmap , new segment to process.
	atomic_set(&data_ready, 0);


	if (offset + size > 2*1024*1024) return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return remap_pfn_range(vma, vma->vm_start, (start + offset) >> PAGE_SHIFT, size, vma->vm_page_prot);
}

/* MSI interrupt handler */
static irqreturn_t crqa_irq_handler(int irq, void *dev_id)
{
	struct cdev *dev = dev_id;
	pr_info("PSD MSI interrupt received on IRQ %d\n", irq);


	//set flag that data are ready now
	atomic_set(&data_ready, 1);

	//wake up any waiting process on those
	wake_up_interruptible(&crqa_waitqueue);

	return IRQ_HANDLED;
}

static unsigned int crqa_poll(struct file *filp, poll_table *wait)
{
	unsigned int mask = 0;

	/* Register wait queue */
	poll_wait(filp, &crqa_waitqueue, wait);

	/* Check if data is ready */
	if (atomic_read(&data_ready)) {
		mask |= POLLIN | POLLRDNORM;  /* Data available to read */
	}

	return mask;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.mmap  = crqa_mmap,
	.poll = crqa_poll,
};


static int crqa_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	uint16_t msi_data;
	uint32_t msi_addr_lo, msi_addr_hi = 0;
	uint8_t msi_flags;
	int msi_cap;


	ret = pci_enable_device(pdev);
	if (ret) return ret;
	printk(KERN_ALERT " CRQA: device enabled\n");

	//check for MSI capability
	msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	printk(KERN_INFO "CRQA: MSI capability at offset: 0x%x\n", msi_cap);
	if (msi_cap) {
		// Read MSI config before enabling
		pci_read_config_byte(pdev, msi_cap + PCI_MSI_FLAGS, &msi_flags);
		printk(KERN_INFO "CRQA: MSI flags before: 0x%02x\n", msi_flags);
		printk(KERN_INFO "CRQA:   - Multiple messages: %d\n",
		       (msi_flags & PCI_MSI_FLAGS_QMASK) >> 1);
		printk(KERN_INFO "CRQA:   - 64-bit capable: %d\n",
		       (msi_flags & PCI_MSI_FLAGS_64BIT) ? 1 : 0);
	}


	ret = pci_request_region(pdev, 0, "crqa");
	if (ret) goto err_disable;

	pdev_global = pdev;

	dev_class = class_create("crqa");
	if (IS_ERR(dev_class)) {
		ret = PTR_ERR(dev_class);
		goto err_region;
	}

	ret = alloc_chrdev_region(&dev_num, 0, 1, CDEV_NAME);
	if (ret) goto err_class;

	cdev_init(&cdev, &fops);
	ret = cdev_add(&cdev, dev_num, 1);
	if (ret) goto err_chrdev;

	dev_device = device_create(dev_class, &pdev->dev, dev_num, NULL, CDEV_NAME);
	if (IS_ERR(dev_device)) {
		ret = PTR_ERR(dev_device);
		goto err_cdev;
	}

	ret = pci_enable_msi(pdev);
	if (ret) {
		dev_warn(&pdev->dev, "MSI not available, falling back to legacy IRQ\n");
	} else {
		printk(KERN_INFO "CRQA: MSI enabled successfully\n");

		// Read MSI config after enabling
		if (msi_cap) {
			pci_read_config_byte(pdev, msi_cap + PCI_MSI_FLAGS, &msi_flags);
			printk(KERN_INFO "CRQA: MSI flags after: 0x%02x (enabled: %d)\n", msi_flags, msi_flags & PCI_MSI_FLAGS_ENABLE);
			pci_read_config_dword(pdev, msi_cap + PCI_MSI_ADDRESS_LO, &msi_addr_lo);
			if (msi_flags & PCI_MSI_FLAGS_64BIT) {
				pci_read_config_dword(pdev, msi_cap + PCI_MSI_ADDRESS_HI, &msi_addr_hi);
			}
			pci_read_config_word(pdev, msi_cap + ((msi_flags & PCI_MSI_FLAGS_64BIT) ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32),&msi_data);

			printk(KERN_INFO "CRQA: MSI address: 0x%08x%08x\n", msi_addr_hi, msi_addr_lo);
			printk(KERN_INFO "CRQA: MSI data: 0x%04x (vector: %d)\n", msi_data, msi_data & 0xFF);
		}

		// Get MSI IRQ information
		printk(KERN_INFO "CRQA: Assigned IRQ: %d\n", pdev->irq);

		// Check which CPU this IRQ is targeting
		struct irq_data *irq_data = irq_get_irq_data(pdev->irq);
		if (irq_data) {
			const struct cpumask *mask = irq_data_get_affinity_mask(irq_data);
			printk(KERN_INFO "CRQA: IRQ affinity: %*pbl\n",
			       cpumask_pr_args(mask));
		}


	}
	printk(KERN_ALERT"LKM: ret: %d  Success pci enable MSI\n", ret);

	ret = request_irq(pdev->irq, crqa_irq_handler, 0, "crqa_pci", pdev);
	if (ret)
	{
		printk(KERN_INFO "error requesting MSI,,,,\n");
	}


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

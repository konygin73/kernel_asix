#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI Assistant & User");
MODULE_DESCRIPTION("Safe and Optimized Char Driver for ASIX AX99100 LPT");
MODULE_VERSION("1.4");

#define DEVICE_NAME "ax99100_lpt"
#define CLASS_NAME  "ax99100"

#define PCI_VENDOR_ID_ASIX         0x125b
#define PCI_DEVICE_ID_ASIX_AX99100 0x9100

static struct pci_device_id ax99100_table[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_ASIX, PCI_DEVICE_ID_ASIX_AX99100) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, ax99100_table);

static dev_t dev_num;
static struct cdev char_dev;
static struct class *driver_class = NULL;

static unsigned long io_base = 0;
static resource_size_t io_len = 0;
static bool is_io_mapped = false;
static DEFINE_MUTEX(ax99100_mutex); // Защита состояния устройства

static ssize_t lpt_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    u8 val;
    int ret;

    if (count == 0) return 0;

    // Конвертируем строку от пользователя в байт (0-255)
    ret = kstrtou8_from_user(buf, count, 0, &val);
    if (ret) {
        pr_err("ax99100_lpt: Invalid string format or value out of range (0-255)\n");
        return ret == -ERANGE ? -EINVAL : ret;
    }

    mutex_lock(&ax99100_mutex);
    if (!is_io_mapped || !io_base) {
        mutex_unlock(&ax99100_mutex);
        pr_err("ax99100_lpt: Error! Device not mapped or already removed\n");
        return -EIO;
    }

    // Запись в базовый порт данных LPT
    outb(val, (unsigned short)io_base);
    pr_info("ax99100_lpt: Written 0x%02X to I/O port 0x%lX\n", val, io_base);
    mutex_unlock(&ax99100_mutex);

    *ppos += count; 
    return count;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .write = lpt_write,
    .llseek = noop_llseek,
};

static int ax99100_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    int target_bars[] = {0, 1};
    unsigned long bar_flags;
    resource_size_t bar_start, bar_len;
    int ret, i, bar;
    bool bar_found = false;

    // Проверяем класс PCI (0x0701 - Parallel Port)
    if ((pdev->class >> 8) != 0x0701) {
        pr_err("ax99100_lpt: PCI device class is not Parallel Port\n");
        return -ENODEV;
    }

    mutex_lock(&ax99100_mutex);
    if (is_io_mapped) {
        mutex_unlock(&ax99100_mutex);
        pr_warn("ax99100_lpt: Another instance is active. Multi-device not supported.\n");
        return -EBUSY;
    }
    mutex_unlock(&ax99100_mutex);

    ret = pci_enable_device(pdev);
    if (ret) {
        pr_err("ax99100_lpt: Failed to enable PCI device\n");
        return ret;
    }

    // Бронируем регионы платы в ядре
    ret = pci_request_regions(pdev, DEVICE_NAME);
    if (ret) {
        pr_err("ax99100_lpt: Failed to request PCI regions\n");
        goto err_disable_pci;
    }

    // Целенаправленный опрос только BAR0 и BAR1
    for (i = 0; i < 2; i++) {
        bar = target_bars[i];
        bar_flags = pci_resource_flags(pdev, bar);
        
        // Регистр должен быть I/O типа и строго 8 байт длиной (стандарт SPP LPT)
        if ((bar_flags & IORESOURCE_IO) && pci_resource_len(pdev, bar) == 8) {
            bar_start = pci_resource_start(pdev, bar);
            bar_len = pci_resource_len(pdev, bar);

            mutex_lock(&ax99100_mutex);
            io_base = (unsigned long)bar_start;
            io_len = bar_len;
            is_io_mapped = true;
            mutex_unlock(&ax99100_mutex);

            pr_info("ax99100_lpt: Automatically detected LPT at BAR%d, I/O port: 0x%llx\n",
                    bar, (unsigned long long)bar_start);
            
            bar_found = true;
            break; // Нашли нужный BAR, досрочно выходим из цикла
        }
    }

    if (!bar_found) {
        pr_err("ax99100_lpt: No valid 8-byte LPT I/O region found at BAR0 or BAR1\n");
        ret = -ENODEV;
        goto err_release_regions;
    }

    return 0;

err_release_regions:
    pci_release_regions(pdev);
err_disable_pci:
    pci_disable_device(pdev);
    return ret;
}

static void ax99100_remove(struct pci_dev *pdev) {
    mutex_lock(&ax99100_mutex);
    is_io_mapped = false;
    io_base = 0;
    io_len = 0;
    mutex_unlock(&ax99100_mutex);

    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pr_info("ax99100_lpt: PCI device removed\n");
}

static struct pci_driver ax99100_driver = {
    .name = DEVICE_NAME,
    .id_table = ax99100_table,
    .probe = ax99100_probe,
    .remove = ax99100_remove,
};

static int __init ax99100_init(void) {
    int ret;
    struct device *dev;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) return ret;

    cdev_init(&char_dev, &fops);
    ret = cdev_add(&char_dev, dev_num, 1);
    if (ret < 0) {
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    driver_class = class_create(CLASS_NAME);
#else
    driver_class = class_create(THIS_MODULE, CLASS_NAME);
#endif

    if (IS_ERR(driver_class)) {
        ret = PTR_ERR(driver_class);
        cdev_del(&char_dev);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    dev = device_create(driver_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        class_destroy(driver_class);
        cdev_del(&char_dev);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    ret = pci_register_driver(&ax99100_driver);
    if (ret < 0) {
        device_destroy(driver_class, dev_num);
        class_destroy(driver_class);
        cdev_del(&char_dev);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    pr_info("ax99100_lpt: Module loaded successfully\n");
    return 0;
}

static void __exit ax99100_exit(void) {
    pci_unregister_driver(&ax99100_driver);
    device_destroy(driver_class, dev_num);
    class_destroy(driver_class);
    cdev_del(&char_dev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("ax99100_lpt: Module unloaded\n");
}

module_init(ax99100_init);
module_exit(ax99100_exit);

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/kernel.h>  // Для kstrtou8_from_user
#include <linux/mutex.h>   // Для mutex

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI Assistant & User");
MODULE_DESCRIPTION("Safe Char Driver for ASIX AX99100 LPT Bits Control");
MODULE_VERSION("1.2");

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
static DEFINE_MUTEX(ax99100_mutex); // Мьютекс для защиты состояния устройства

static ssize_t lpt_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    u8 val;
    int ret;

    pr_info("ax99100_lpt: lpt_write entry (count=%zu, ppos=%lld)\n", count, *ppos);

    if (count == 0) return 0;

    // Безопасное чтение и преобразование строки в 8-битное число (0-255)
    ret = kstrtou8_from_user(buf, count, 0, &val);
    if (ret) {
        pr_err("ax99100_lpt: Invalid string format or value out of range (0-255)\n");
        return ret == -ERANGE ? -EINVAL : ret;
    }

    // Защита от гонки данных с функцией remove
    mutex_lock(&ax99100_mutex);
    if (!is_io_mapped || !io_base) {
        mutex_unlock(&ax99100_mutex);
        pr_err("ax99100_lpt: Error! Device not mapped or already removed\n");
        return -EIO;
    }

    // Приведение к unsigned short для соответствия сигнатуре outb
    outb(val, (unsigned short)io_base);
    pr_info("ax99100_lpt: Written 0x%02X to I/O port 0x%lX\n", val, io_base);
    mutex_unlock(&ax99100_mutex);

    *ppos += count; // Стандартное поведение для файловых операций
    return count;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .write = lpt_write,
    .llseek = noop_llseek,
};

static int ax99100_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    int bar;
    unsigned long bar_flags;
    resource_size_t bar_start, bar_len;

    // 0x0701 - класс Parallel Port
    if ((pdev->class >> 8) != 0x0701) {
        return -ENODEV;
    }

    pr_info("ax99100_lpt: PCI device found!\n");

    mutex_lock(&ax99100_mutex);
    if (is_io_mapped) {
        mutex_unlock(&ax99100_mutex);
        pr_warn("ax99100_lpt: Another instance is already mapped. Multi-device not supported yet.\n");
        return -EBUSY;
    }
    mutex_unlock(&ax99100_mutex);

    if (pci_enable_device(pdev)) {
        pr_err("ax99100_lpt: Failed to enable PCI device\n");
        return -EIO;
    }

    for (bar = 0; bar < 6; bar++) {
        bar_flags = pci_resource_flags(pdev, bar);
        if (bar_flags & IORESOURCE_IO) {
            bar_start = pci_resource_start(pdev, bar);
            bar_len = pci_resource_len(pdev, bar);

            if (bar_len == 0)
                continue;

            if (!request_region(bar_start, bar_len, DEVICE_NAME)) {
                pr_err("ax99100_lpt: I/O region 0x%llx busy\n", (unsigned long long)bar_start);
                continue;
            }

            mutex_lock(&ax99100_mutex);
            io_base = (unsigned long)bar_start;
            io_len = bar_len;
            is_io_mapped = true;
            mutex_unlock(&ax99100_mutex);

            pr_info("ax99100_lpt: Using BAR%d at I/O port: 0x%llx (length: %llu)\n",
                    bar, (unsigned long long)bar_start, (unsigned long long)bar_len);
            return 0; // Успешный выход из probe
        }
    }

    pr_err("ax99100_lpt: No valid I/O BAR found or all regions busy\n");
    pci_disable_device(pdev);
    return -ENODEV;
}

static void ax99100_remove(struct pci_dev *pdev) {
    unsigned long base_to_release = 0;
    resource_size_t len_to_release = 0;

    mutex_lock(&ax99100_mutex);
    if (is_io_mapped && io_base && io_len) {
        // Сначала снимаем флаг, чтобы write сразу видел невалидность
        is_io_mapped = false;
        base_to_release = io_base;
        len_to_release = io_len;
        io_base = 0;
        io_len = 0;
    }
    mutex_unlock(&ax99100_mutex);

    if (base_to_release && len_to_release) {
        release_region(base_to_release, len_to_release);
    }

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
    pci_unregister_driver(&ax99100_driver); // Это вызовет ax99100_remove для всех устройств
    device_destroy(driver_class, dev_num);
    class_destroy(driver_class);
    cdev_del(&char_dev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("ax99100_lpt: Module unloaded\n");
}

module_init(ax99100_init);
module_exit(ax99100_exit);

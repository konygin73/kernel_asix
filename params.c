
#include "pc_mod.h"
#include <linux/log2.h>
#include <linux/random.h>






#include <linux/proc_fs.h> // Обязательный заголовок

static struct proc_dir_entry *proc_file = NULL;

// Функция, которая вызывается при чтении файла /proc/ax99100_stat
static ssize_t proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[64];
    int len;

    // Формируем текст, который увидит пользователь
    len = snprintf(kbuf, sizeof(kbuf), "AX99100 I/O Base: 0x%lX\nStatus: %s\n", 
                   io_base, is_io_mapped ? "Active" : "Inactive");

    // Стандартный патч для предотвращения зацикливания cat
    if (*ppos > 0 || count < len) return 0;

    if (copy_to_user(buf, kbuf, len)) return -EFAULT;

    *ppos += len;
    return len;
}

// Структура операций для /proc
static const struct proc_ops proc_fops = {
    .proc_read = proc_read,
};

{

  proc_file = proc_create("ax99100_stat", 0444, NULL, &proc_fops);
if (!proc_file) {
    pr_err("ax99100_lpt: Could not create /proc entry\n");
    // Тут можно не прерывать init, если /proc не критичен
}

if (proc_file) {
    remove_proc_entry("ax99100_stat", NULL);
}

}



#include <linux/sysfs.h> // Обязательный заголовок

// Функция чтения из /sys (вызывается при cat)
static ssize_t pins_show(struct device *dev, struct device_attribute *attr, char *buf) {
    // В sysfs buf гарантированно имеет размер PAGE_SIZE (4096 байт)
    return snprintf(buf, PAGE_SIZE, "0x%lX\n", io_base ? (unsigned long)inb(io_base) : 0);
}

/// Функция записи в /sys (вызывается при echo)
static ssize_t pins_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    long val;
    int ret;

    ret = kstrtol(buf, 0, &val);
    if (ret < 0 || val < 0 || val > 255) return -EINVAL;

    if (is_io_mapped && io_base) {
        outb((u8)val, io_base);
        pr_info("ax99100_lpt: [sysfs] Written 0x%02X\n", (u8)val);
    } else {
        return -EIO;
    }

    return count; // Обязательно возвращаем количество обработанных байт
}

static DEVICE_ATTR_RW(pins);







#include <linux/moduleparam.h>

unsigned int g_fifo_size = 64;
unsigned int g_num_events = 200;
unsigned int g_interval_us = 1000;
unsigned int g_consumer_type = 0;

// sys/module/[имя_вашего_модуля]/parameters/fifo_size
static int param_set_fifo_size(const char *val, const struct kernel_param *kp) {
  unsigned int fifo_size;
  int ret;

  ret = kstrtouint(val, 0, &fifo_size);
  if (ret)
    return ret;

  if (fifo_size < 4 || fifo_size > 1024 || !is_power_of_2(fifo_size)) {
    pr_err("Invalid fifo_size %u. Размер kfifo (количество слотов, степень "
           "двойки [4..1024])\n",
           fifo_size);
    return -EINVAL;
  }

  g_fifo_size = fifo_size;
  return ret;
}
static const struct kernel_param_ops fifo_size_ops = {
    .set = param_set_fifo_size,
    .get = param_get_uint,
};
module_param_cb(fifo_size, &fifo_size_ops, &g_fifo_size, 0400);
MODULE_PARM_DESC(fifo_size,
                 "fifo_size (количество слотов, степень двойки [4..1024])");

static int param_set_num_events(const char *val,
                                const struct kernel_param *kp) {
  unsigned int num_events;
  int ret;

  ret = kstrtouint(val, 0, &num_events);
  if (ret)
    return ret;

  if (num_events < 1 || num_events > 50000) {
    pr_err("Invalid num_events %u. Размер num_events 1...50_000\n", num_events);
    return -EINVAL;
  }

  g_num_events = num_events;
  return ret;
}
static const struct kernel_param_ops num_events_ops = {
    .set = param_set_num_events,
    .get = param_get_uint,
};
module_param_cb(num_events, &num_events_ops, &g_num_events, 0600);
MODULE_PARM_DESC(num_events,
                 "num_events Количество генерируемых событий [1..50000]");

static int param_set_interval_us(const char *val,
                                 const struct kernel_param *kp) {
  unsigned int interval_us;
  int ret;

  ret = kstrtouint(val, 0, &interval_us);
  if (ret)
    return ret;

  if (interval_us < 100 || interval_us > 1000000) {
    pr_err("Invalid interval_us %u. Интервал таймера в микросекундах "
           "[100..1000000]\n",
           interval_us);
    return -EINVAL;
  }

  g_interval_us = interval_us;
  return ret;
}
static const struct kernel_param_ops interval_us_ops = {
    .set = param_set_interval_us,
    .get = param_get_uint,
};
module_param_cb(interval_us, &interval_us_ops, &g_interval_us, 0600);
MODULE_PARM_DESC(interval_us,
                 "interval_us Интервал таймера в микросекундах [100..1000000]");

static int param_set_consumer_type(const char *val,
                                   const struct kernel_param *kp) {
  unsigned int consumer_type;
  int ret;

  ret = kstrtouint(val, 0, &consumer_type);
  if (ret)
    return ret;

  if (consumer_type > 1) {
    pr_err(
        "Invalid consumer_type %u. Тип consumer: 0 - tasklet, 1 - workqueue\n",
        consumer_type);
    return -EINVAL;
  }

  g_consumer_type = consumer_type;
  return ret;
}
static const struct kernel_param_ops consumer_type_ops = {
    .set = param_set_consumer_type,
    .get = param_get_uint,
};
module_param_cb(consumer_type, &consumer_type_ops, &g_consumer_type, 0600);
MODULE_PARM_DESC(consumer_type,
                 "consumer_type Тип consumer: 0 - tasklet, 1 - workqueue");

static int param_set_run(const char *val, const struct kernel_param *kp) {
  int v, ret;
  ktime_t ktime;
  unsigned long timeout_jiffies;
  unsigned long long total_time_us;

  ret = kstrtoint(val, 10, &v);
  if (ret || v != 1)
    return -EINVAL;

  if (atomic_cmpxchg(&ctx.is_running, 0, 1) != 0)
    return -EBUSY;

  ctx.num_events = g_num_events;
  ctx.interval_us = g_interval_us;
  ctx.consumer_type = g_consumer_type;

  kfifo_reset(&ctx.fifo);
  atomic_set(&ctx.produced, 0);
  atomic_set(&ctx.dropped, 0);
  atomic_set(&ctx.consumed, 0);
  atomic64_set(&ctx.sum, 0);
  ctx.last_value = 0;

  if (ctx.consumer_type == 0) {
    tasklet_setup(&ctx.tasklet, tasklet_consumer);
  } else {
    INIT_WORK(&ctx.work, work_consumer);

    ctx.wq = create_singlethread_workqueue("pc_demo_wq");
    if (!ctx.wq) {
      atomic_set(&ctx.is_running, 0);
      return -ENOMEM;
    }
  }

  ktime = ktime_set(0, ctx.interval_us * 1000);
  hrtimer_start(&ctx.timer, ktime, HRTIMER_MODE_REL);

  total_time_us = (unsigned long long)ctx.num_events * ctx.interval_us;
  timeout_jiffies =
      msecs_to_jiffies((unsigned long)(total_time_us / 1000) + 1000);

  ret = wait_event_timeout(
      ctx.wait_queue,
      (atomic_read(&ctx.produced) + atomic_read(&ctx.dropped) >=
       ctx.num_events),
      timeout_jiffies);

  if (ret == 0) {
    pr_warn("Таймаут ожидания завершения теста\n");
    hrtimer_cancel(&ctx.timer);
  }

  if (ctx.consumer_type == 0) {
    tasklet_kill(&ctx.tasklet);
  } else {
    if (ctx.wq) {
      flush_workqueue(ctx.wq);
      destroy_workqueue(ctx.wq);
      ctx.wq = NULL;
    }
  }

  atomic_set(&ctx.is_running, 0);
  return 0;
}
static const struct kernel_param_ops param_ops_run = {
    .set = param_set_run,
};
module_param_cb(run, &param_ops_run, NULL, 0200);
MODULE_PARM_DESC(run, "Запустить тест (echo 1 > .../run)");

static int param_get_result(char *buffer, const struct kernel_param *kp) {
  int p = atomic_read(&ctx.produced);
  int c = atomic_read(&ctx.consumed);
  int d = atomic_read(&ctx.dropped);
  const char *ctype = (ctx.consumer_type == 0) ? "tasklet" : "workqueue";

  if (c < p) {
    return sysfs_emit(
        buffer,
        "produced=%d consumed=%d dropped=%d consumer=%s warn: lost=%d\n", p, c,
        d, ctype, p - c);
  } else {
    return sysfs_emit(buffer,
                      "produced=%d consumed=%d dropped=%d consumer=%s ok\n", p,
                      c, d, ctype);
  }
}
static const struct kernel_param_ops param_ops_result = {
    .get = param_get_result,
};
module_param_cb(result, &param_ops_result, NULL, 0444);
MODULE_PARM_DESC(result, "Результат последнего запуска");

static int param_get_stats(char *buffer, const struct kernel_param *kp) {
  int p = atomic_read(&ctx.produced);
  int c = atomic_read(&ctx.consumed);
  int d = atomic_read(&ctx.dropped);
  u64 s = atomic64_read(&ctx.sum);
  unsigned int last = ctx.last_value;
  unsigned int avg = (c > 0) ? (unsigned int)(s / c) : 0;

  return sysfs_emit(
      buffer, "produced=%d consumed=%d dropped=%d sum=%llu last=%u avg=%u\n", p,
      c, d, (unsigned long long)s, last, avg);
}
static const struct kernel_param_ops param_ops_stats = {
    .get = param_get_stats,
};
module_param_cb(stats, &param_ops_stats, NULL, 0444);
MODULE_PARM_DESC(stats, "Статистика produced/consumed/dropped/sum/last/avg");

static int param_set_reset(const char *val, const struct kernel_param *kp) {
  int v, ret;

  ret = kstrtoint(val, 10, &v);
  if (ret || v != 1)
    return -EINVAL;

  if (atomic_read(&ctx.is_running)) {
    hrtimer_cancel(&ctx.timer);

    if (ctx.consumer_type == 0) {
      tasklet_kill(&ctx.tasklet);
    } else {
      if (ctx.wq) {
        flush_workqueue(ctx.wq);
        destroy_workqueue(ctx.wq);
        ctx.wq = NULL;
      }
    }

    atomic_set(&ctx.is_running, 0);
  }

  kfifo_reset(&ctx.fifo);
  atomic_set(&ctx.produced, 0);
  atomic_set(&ctx.dropped, 0);
  atomic_set(&ctx.consumed, 0);
  atomic64_set(&ctx.sum, 0);
  ctx.last_value = 0;

  return 0;
}
static const struct kernel_param_ops param_ops_reset = {
    .set = param_set_reset,
};
module_param_cb(reset, &param_ops_reset, NULL, 0200);
MODULE_PARM_DESC(reset, "Сбросить очередь и статистику (echo 1 > .../reset)");

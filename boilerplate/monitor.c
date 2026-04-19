/*
 * monitor.c — Kernel-Space Memory Monitor LKM
 *
 * Tracks registered container PIDs, checks RSS periodically,
 * fires soft-limit warnings and hard-limit kills.
 *
 * Control device: /dev/container_monitor
 * Build: see Makefile (obj-m := monitor.o)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/pid.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Container Memory Monitor");
MODULE_VERSION("1.0");

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_MS 1000

/* ── per-container entry in kernel linked list ── */
struct cmon_entry {
    struct list_head list;
    int              pid;
    long             soft_mib;
    long             hard_mib;
    int              soft_warned;
    char             id[64];
};

static LIST_HEAD(g_clist);
static DEFINE_MUTEX(g_clist_lock);

static dev_t               g_devno;
static struct cdev         g_cdev;
static struct class       *g_class;
static struct task_struct *g_monitor_thread;

/* ── get RSS in bytes for a host PID ── */
static long get_rss_bytes(int pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return 0;

    rss = (long)get_mm_rss(mm) * PAGE_SIZE;
    mmput(mm);
    return rss;
}

/* ── monitor kthread ── */
static int monitor_fn(void *data)
{
    /* FIX 1: declare loop variables at top of block (kernel C89 style) */
    struct cmon_entry *entry, *tmp;
    long rss, rss_mib;
    (void)data;

    while (!kthread_should_stop()) {
        msleep(CHECK_INTERVAL_MS);

        mutex_lock(&g_clist_lock);
        list_for_each_entry_safe(entry, tmp, &g_clist, list) {
            rss = get_rss_bytes(entry->pid);

            if (rss < 0) {
                pr_info("container_monitor: PID %d (%s) gone, removing\n",
                        entry->pid, entry->id);
                list_del(&entry->list);
                kfree(entry);
                continue;
            }

            rss_mib = rss >> 20;

            if (rss_mib >= entry->hard_mib) {
                pr_warn("container_monitor: [%s] pid=%d hard limit %ldMiB exceeded "
                        "(rss=%ldMiB) -- sending SIGKILL\n",
                        entry->id, entry->pid, entry->hard_mib, rss_mib);
                kill_pid(find_vpid(entry->pid), SIGKILL, 1);
                list_del(&entry->list);
                kfree(entry);
                continue;
            }

            if (!entry->soft_warned && rss_mib >= entry->soft_mib) {
                pr_warn("container_monitor: [%s] pid=%d soft limit %ldMiB exceeded "
                        "(rss=%ldMiB) -- warning\n",
                        entry->id, entry->pid, entry->soft_mib, rss_mib);
                entry->soft_warned = 1;
            }
        }
        mutex_unlock(&g_clist_lock);
    }
    return 0;
}

/* ── ioctl handler ── */
static long mon_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    /* FIX 2: declare all variables at top of function, not inside switch cases */
    struct container_reg   reg;
    struct container_unreg unreg;
    struct cmon_entry     *e;
    struct cmon_entry     *entry, *tmp;
    (void)file;

    switch (cmd) {
    case CONTAINER_MONITOR_REGISTER:
        if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
            return -EFAULT;

        e = kzalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;

        e->pid       = reg.pid;
        e->soft_mib  = reg.soft_mib;
        e->hard_mib  = reg.hard_mib;
        strncpy(e->id, reg.id, sizeof(e->id) - 1);

        mutex_lock(&g_clist_lock);
        list_add_tail(&e->list, &g_clist);
        mutex_unlock(&g_clist_lock);

        pr_info("container_monitor: registered [%s] pid=%d soft=%dMiB hard=%dMiB\n",
                e->id, e->pid, reg.soft_mib, reg.hard_mib);
        return 0;

    case CONTAINER_MONITOR_UNREGISTER:
        if (copy_from_user(&unreg, (void __user *)arg, sizeof(unreg)))
            return -EFAULT;

        mutex_lock(&g_clist_lock);
        list_for_each_entry_safe(entry, tmp, &g_clist, list) {
            if (entry->pid == unreg.pid) {
                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }
        mutex_unlock(&g_clist_lock);
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations mon_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = mon_ioctl,
};

/* ── module init ── */
static int __init mon_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&g_devno, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("container_monitor: alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&g_cdev, &mon_fops);
    ret = cdev_add(&g_cdev, g_devno, 1);
    if (ret < 0) {
        unregister_chrdev_region(g_devno, 1);
        return ret;
    }

    /* FIX 3: kernel 6.4+ removed THIS_MODULE from class_create() */
    g_class = class_create(DEVICE_NAME);
    if (IS_ERR(g_class)) {
        cdev_del(&g_cdev);
        unregister_chrdev_region(g_devno, 1);
        return PTR_ERR(g_class);
    }

    device_create(g_class, NULL, g_devno, NULL, DEVICE_NAME);

    g_monitor_thread = kthread_run(monitor_fn, NULL, "cmon_thread");
    if (IS_ERR(g_monitor_thread)) {
        device_destroy(g_class, g_devno);
        class_destroy(g_class);
        cdev_del(&g_cdev);
        unregister_chrdev_region(g_devno, 1);
        return PTR_ERR(g_monitor_thread);
    }

    pr_info("container_monitor: loaded, device /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* ── module exit ── */
static void __exit mon_exit(void)
{
    /* declare variables at top */
    struct cmon_entry *entry, *tmp;

    kthread_stop(g_monitor_thread);

    mutex_lock(&g_clist_lock);
    list_for_each_entry_safe(entry, tmp, &g_clist, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&g_clist_lock);

    device_destroy(g_class, g_devno);
    class_destroy(g_class);
    cdev_del(&g_cdev);
    unregister_chrdev_region(g_devno, 1);
    pr_info("container_monitor: unloaded\n");
}

module_init(mon_init);
module_exit(mon_exit);

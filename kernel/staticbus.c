// SPDX-License-Identifier: GPL-2.0
/*
 * staticbus: bounded byte-stream bridge for externally captured bus samples.
 *
 * The module deliberately does not pretend that software can observe arbitrary
 * CPU package pins. A hardware capture source (for example, an FPGA, logic
 * analyzer, or platform-specific driver) supplies bytes by writing to this
 * character device. The GTK frontend reads the same stream.
 */
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "staticbus_ioctl.h"

#define STATICBUS_NAME "staticbus"
#define STATICBUS_DEVICE_NAME "staticbus0"
#define STATICBUS_DEFAULT_BUFFER_SIZE (64U * 1024U)
#define STATICBUS_MAX_BUFFER_SIZE (16U * 1024U * 1024U)

static unsigned int buffer_size = STATICBUS_DEFAULT_BUFFER_SIZE;
module_param(buffer_size, uint, 0444);
MODULE_PARM_DESC(buffer_size, "Requested byte FIFO size (default 65536; max 16777216)");

static dev_t sb_devno;
static struct cdev sb_cdev;
static struct class *sb_class;
static struct device *sb_device;
static DECLARE_KFIFO_PTR(sb_fifo, unsigned char);
static DEFINE_MUTEX(sb_lock);
static DECLARE_WAIT_QUEUE_HEAD(sb_readq);
static DECLARE_WAIT_QUEUE_HEAD(sb_writeq);
static u64 sb_bytes_written;
static u64 sb_bytes_read;

/* Called by wait_event* without sb_lock held. */
static bool sb_has_data(void)
{
    bool ready;
    mutex_lock(&sb_lock);
    ready = !kfifo_is_empty(&sb_fifo);
    mutex_unlock(&sb_lock);
    return ready;
}

static bool sb_has_space(void)
{
    bool ready;
    mutex_lock(&sb_lock);
    ready = !kfifo_is_full(&sb_fifo);
    mutex_unlock(&sb_lock);
    return ready;
}

static int sb_open(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

static int sb_release(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

static ssize_t sb_read(struct file *file, char __user *user_buf,
                       size_t count, loff_t *position)
{
    unsigned char *tmp;
    unsigned int copied;
    ssize_t result;

    (void)position;
    if (count == 0)
        return 0;

    tmp = kmalloc(min_t(size_t, count, PAGE_SIZE), GFP_KERNEL);
    if (!tmp)
        return -ENOMEM;

    for (;;) {
        mutex_lock(&sb_lock);
        copied = kfifo_out(&sb_fifo, tmp,
                           min_t(size_t, count, PAGE_SIZE));
        if (copied)
            sb_bytes_read += copied;
        mutex_unlock(&sb_lock);

        if (copied) {
            if (copy_to_user(user_buf, tmp, copied)) {
                result = -EFAULT;
            } else {
                result = (ssize_t)copied;
            }
            wake_up_interruptible(&sb_writeq);
            kfree(tmp);
            return result;
        }

        if (file->f_flags & O_NONBLOCK) {
            kfree(tmp);
            return -EAGAIN;
        }
        result = wait_event_interruptible(sb_readq, sb_has_data());
        if (result) {
            kfree(tmp);
            return result;
        }
    }
}

static ssize_t sb_write(struct file *file, const char __user *user_buf,
                        size_t count, loff_t *position)
{
    unsigned char *tmp;
    size_t total = 0;
    ssize_t result = 0;

    (void)position;
    if (count == 0)
        return 0;

    tmp = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!tmp)
        return -ENOMEM;

    while (total < count) {
        size_t chunk = min_t(size_t, count - total, PAGE_SIZE);
        unsigned int inserted;

        if (copy_from_user(tmp, user_buf + total, chunk)) {
            result = total ? (ssize_t)total : -EFAULT;
            goto out;
        }

        for (;;) {
            unsigned int available;

            mutex_lock(&sb_lock);
            available = kfifo_avail(&sb_fifo);
            inserted = kfifo_in(&sb_fifo, tmp,
                                min_t(unsigned int, available,
                                      (unsigned int)chunk));
            sb_bytes_written += inserted;
            mutex_unlock(&sb_lock);

            if (inserted) {
                total += inserted;
                wake_up_interruptible(&sb_readq);
                break;
            }

            if (file->f_flags & O_NONBLOCK) {
                result = total ? (ssize_t)total : -EAGAIN;
                goto out;
            }

            result = wait_event_interruptible(sb_writeq, sb_has_space());
            if (result) {
                result = total ? (ssize_t)total : result;
                goto out;
            }
        }
    }

    result = (ssize_t)total;
out:
    kfree(tmp);
    return result;
}

static __poll_t sb_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;

    poll_wait(file, &sb_readq, wait);
    poll_wait(file, &sb_writeq, wait);

    mutex_lock(&sb_lock);
    if (!kfifo_is_empty(&sb_fifo))
        mask |= POLLIN | POLLRDNORM;
    if (!kfifo_is_full(&sb_fifo))
        mask |= POLLOUT | POLLWRNORM;
    mutex_unlock(&sb_lock);

    return mask;
}

static long sb_ioctl(struct file *file, unsigned int command,
                     unsigned long argument)
{
    struct staticbus_stats stats = { 0 };

    (void)file;
    if (command != STATICBUS_IOC_GET_STATS)
        return -ENOTTY;

    mutex_lock(&sb_lock);
    stats.abi_version = STATICBUS_ABI_VERSION;
    stats.buffer_capacity = kfifo_size(&sb_fifo);
    stats.buffer_used = kfifo_len(&sb_fifo);
    stats.bytes_written = sb_bytes_written;
    stats.bytes_read = sb_bytes_read;
    mutex_unlock(&sb_lock);

    if (copy_to_user((void __user *)argument, &stats, sizeof(stats)))
        return -EFAULT;
    return 0;
}

static const struct file_operations sb_fops = {
    .owner = THIS_MODULE,
    .open = sb_open,
    .release = sb_release,
    .read = sb_read,
    .write = sb_write,
    .poll = sb_poll,
    .unlocked_ioctl = sb_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = compat_ptr_ioctl,
#endif
    .llseek = no_llseek,
};

static int __init sb_init(void)
{
    int ret;

    if (buffer_size < PAGE_SIZE || buffer_size > STATICBUS_MAX_BUFFER_SIZE) {
        pr_err(STATICBUS_NAME ": buffer_size must be between %lu and %u bytes\n",
               PAGE_SIZE, STATICBUS_MAX_BUFFER_SIZE);
        return -EINVAL;
    }

    ret = kfifo_alloc(&sb_fifo, buffer_size, GFP_KERNEL);
    if (ret)
        return ret;

    ret = alloc_chrdev_region(&sb_devno, 0, 1, STATICBUS_NAME);
    if (ret)
        goto err_fifo;

    cdev_init(&sb_cdev, &sb_fops);
    sb_cdev.owner = THIS_MODULE;
    ret = cdev_add(&sb_cdev, sb_devno, 1);
    if (ret)
        goto err_region;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    sb_class = class_create(STATICBUS_NAME);
#else
    sb_class = class_create(THIS_MODULE, STATICBUS_NAME);
#endif
    if (IS_ERR(sb_class)) {
        ret = PTR_ERR(sb_class);
        goto err_cdev;
    }

    sb_device = device_create(sb_class, NULL, sb_devno, NULL,
                              STATICBUS_DEVICE_NAME);
    if (IS_ERR(sb_device)) {
        ret = PTR_ERR(sb_device);
        goto err_class;
    }

    pr_info(STATICBUS_NAME ": registered /dev/%s (FIFO capacity %u bytes)\n",
            STATICBUS_DEVICE_NAME, kfifo_size(&sb_fifo));
    return 0;

err_class:
    class_destroy(sb_class);
err_cdev:
    cdev_del(&sb_cdev);
err_region:
    unregister_chrdev_region(sb_devno, 1);
err_fifo:
    kfifo_free(&sb_fifo);
    return ret;
}

static void __exit sb_exit(void)
{
    device_destroy(sb_class, sb_devno);
    class_destroy(sb_class);
    cdev_del(&sb_cdev);
    unregister_chrdev_region(sb_devno, 1);
    kfifo_free(&sb_fifo);
    wake_up_interruptible(&sb_readq);
    wake_up_interruptible(&sb_writeq);
    pr_info(STATICBUS_NAME ": unloaded\n");
}

module_init(sb_init);
module_exit(sb_exit);

MODULE_AUTHOR("Jordan Legare / staticbus contributors");
MODULE_DESCRIPTION("Character-device byte-stream bridge for external bus capture sources");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");

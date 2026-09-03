// kvstore.c - kernel key-value store character device driver

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/mutex.h>

#define DEVICE_NAME "kvstore"
#define MAX_KEY_LEN 64
#define MAX_VAL_LEN 256

#define KVSTORE_MAGIC 'k'
#define KVSTORE_CLEAR  _IO(KVSTORE_MAGIC, 1)
#define KVSTORE_COUNT  _IOR(KVSTORE_MAGIC, 2, int)
#define KVSTORE_DELETE _IOW(KVSTORE_MAGIC, 3, char[MAX_KEY_LEN])

static DEFINE_MUTEX(kv_mutex);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nehoray");
MODULE_DESCRIPTION("Simple kernel key-value store");

struct kv_entry
{
    char key[MAX_KEY_LEN];
    char value[MAX_VAL_LEN];
    struct list_head list;
};

static LIST_HEAD(kv_list);
static int entry_count = 0;

static int major_number;
static struct class *kvstore_class;
static struct cdev kvstore_cdev;

static int kvstore_open(struct inode *inode, struct file *file)
{
    pr_info("kvstore: device opened\n");
    return 0;
}

static int kvstore_release(struct inode *inode, struct file *file)
{
    pr_info("kvstore: device closed\n");
    return 0;
}

static struct kv_entry *find_entry(const char *key)
{
    struct kv_entry *entry;

    list_for_each_entry(entry, &kv_list, list)
    {
        if (0 == strcmp(entry->key, key))
        {
            return entry;
        }
    }
    return NULL;
}

static ssize_t kvstore_write(struct file *file, const char __user *buf,
                             size_t count, loff_t *pos)
{
    char kbuf[MAX_KEY_LEN + MAX_VAL_LEN + 2];
    char *eq_pos;
    struct kv_entry *entry;
    size_t len;

    len = min_t(count, sizeof(kbuf) - 1);

    if (copy_from_user(kbuf, buf, len))
    {
        return -EFAULT;
    }

    kbuf[len] = '\0';

    if (len > 0 && '\n' == kbuf[len - 1])
    {
        kbuf[len - 1] = '\0';
    }

    eq_pos = strchr(kbuf, '=');
    if (!eq_pos)
    {
        pr_err("kvstore: invalid format, expected key=value\n");
        return -EINVAL;
    }

    *eq_pos = '\0';
    eq_pos++;

    if (0 == strlen(kbuf) || strlen(kbuf) >= MAX_KEY_LEN)
    {
        pr_err("kvstore: key too long or empty\n");
        return -EINVAL;
    }
    if (strlen(eq_pos) >= MAX_VAL_LEN)
    {
        pr_err("kvstore: value too long\n");
        return -EINVAL;
    }

    mutex_lock(&kv_mutex);

    entry = find_entry(kbuf);
    if (entry)
    {
        strscpy(entry->value, eq_pos, MAX_VAL_LEN);
        mutex_unlock(&kv_mutex);
        pr_info("kvstore: updated key '%s'\n", kbuf);
        return count;
    }

    entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
    {
        mutex_unlock(&kv_mutex);
        return -ENOMEM;
    }

    strscpy(entry->key, kbuf, MAX_KEY_LEN);
    strscpy(entry->value, eq_pos, MAX_VAL_LEN);
    list_add_tail(&entry->list, &kv_list);
    entry_count++;

    pr_info("kvstore: added key '%s' = '%s' (total: %d)\n",
            entry->key, entry->value, entry_count);

    mutex_unlock(&kv_mutex);
    return count;
}

static ssize_t kvstore_read(struct file *file, char __user *buf,
                            size_t count, loff_t *pos)
{
    struct kv_entry *entry;
    char *kbuf;
    size_t total_len = 0;
    size_t remaining;
    int ret;

    mutex_lock(&kv_mutex);

    list_for_each_entry(entry, &kv_list, list)
    {
        total_len += strlen(entry->key) + 1 + strlen(entry->value) + 1;
    }

    if (0 == total_len || *pos >= total_len)
    {
        mutex_unlock(&kv_mutex);
        return 0;
    }

    kbuf = kmalloc(total_len + 1, GFP_KERNEL);
    if (!kbuf)
    {
        mutex_unlock(&kv_mutex);
        return -ENOMEM;
    }

    kbuf[0] = '\0';

    list_for_each_entry(entry, &kv_list, list)
    {
        strcat(kbuf, entry->key);
        strcat(kbuf, "=");
        strcat(kbuf, entry->value);
        strcat(kbuf, "\n");
    }

    remaining = total_len - *pos;
    if (remaining > count)
    {
        remaining = count;
    }

    mutex_unlock(&kv_mutex);

    ret = copy_to_user(buf, kbuf + *pos, remaining);
    kfree(kbuf);

    if (ret)
    {
        return -EFAULT;
    }

    *pos += remaining;
    return remaining;
}

static void clearList(void)
{
    struct kv_entry *entry;
    struct kv_entry *temp;
    
    mutex_lock(&kv_mutex);
    list_for_each_entry_safe(entry, temp, &kv_list, list)
    {
        list_del(&entry->list);
        kfree(entry);
    }
    entry_count = 0;
    mutex_unlock(&kv_mutex);
}

static void removeKey(const char *keyToDelete)
{
    struct kv_entry *entry;
    struct kv_entry *temp;
    
    mutex_lock(&kv_mutex);
    list_for_each_entry_safe(entry, temp, &kv_list, list)
    {
        if (0 == strcmp(keyToDelete, entry->key))
        {
            list_del(&entry->list);
            kfree(entry);
            entry_count--;
            break;
        }
    }
    mutex_unlock(&kv_mutex);
}

static long kvstore_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd)
    {
        case KVSTORE_CLEAR:
            clearList();
            pr_info("kvstore: ioctl clear called\n");
            break;
            
        case KVSTORE_COUNT:
        {
            int temp_count;
            
            mutex_lock(&kv_mutex);
            temp_count = entry_count;
            mutex_unlock(&kv_mutex);
            
            if (copy_to_user((int __user *)arg, &temp_count, sizeof(int)))
            {
                return -EFAULT;
            }
            pr_info("kvstore: ioctl count called\n");
            break;
        }
            
        case KVSTORE_DELETE:
        {
            char kbuf[MAX_KEY_LEN];
            if (copy_from_user(kbuf, (char __user *)arg, sizeof(kbuf)))
            {
                return -EFAULT;
            }
            kbuf[MAX_KEY_LEN - 1] = '\0';
            removeKey(kbuf);
            
            pr_info("kvstore: ioctl delete called\n");
            break;
        }
        default:
            pr_err("kvstore: unknown ioctl command\n");
            return -EINVAL;
    }
    return 0;
}

static const struct file_operations kvstore_fops =
{
    .owner = THIS_MODULE,
    .open = kvstore_open,
    .release = kvstore_release,
    .read = kvstore_read,
    .write = kvstore_write,
    .unlocked_ioctl = kvstore_unlocked_ioctl,
};


static int __init kvstore_init(void)
{
    dev_t dev;
    int ret;

    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret < 0)
    {
        pr_err("kvstore: failed to allocate major number\n");
        return ret;
    }
    major_number = MAJOR(dev);

    cdev_init(&kvstore_cdev, &kvstore_fops);
    kvstore_cdev.owner = THIS_MODULE;
    ret = cdev_add(&kvstore_cdev, dev, 1);
    if (ret < 0)
    {
        unregister_chrdev_region(dev, 1);
        pr_err("kvstore: failed to add cdev\n");
        return ret;
    }

    kvstore_class = class_create(DEVICE_NAME);
    if (IS_ERR(kvstore_class))
    {
        cdev_del(&kvstore_cdev);
        unregister_chrdev_region(dev, 1);
        pr_err("kvstore: failed to create class\n");
        return PTR_ERR(kvstore_class);
    }

    if (IS_ERR(device_create(kvstore_class, NULL, dev, NULL, DEVICE_NAME)))
    {
        class_destroy(kvstore_class);
        cdev_del(&kvstore_cdev);
        unregister_chrdev_region(dev, 1);
        pr_err("kvstore: failed to create device\n");
        return -1;
    }

    pr_info("kvstore: loaded (major=%d)\n", major_number);
    return 0;
}

static void __exit kvstore_exit(void)
{
    struct kv_entry *entry, *tmp;
    dev_t dev = MKDEV(major_number, 0);

    mutex_lock(&kv_mutex);
    list_for_each_entry_safe(entry, tmp, &kv_list, list)
    {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&kv_mutex);

    device_destroy(kvstore_class, dev);
    class_destroy(kvstore_class);
    cdev_del(&kvstore_cdev);
    unregister_chrdev_region(dev, 1);

    pr_info("kvstore: unloaded\n");
}

module_init(kvstore_init);
module_exit(kvstore_exit);
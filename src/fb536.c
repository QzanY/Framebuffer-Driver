#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include "fb536.h"

static int fba_major = 0;
static int numminors = 4;  
static int width = 1000; 
static int height = 1000; 

module_param(numminors, int, 0);
module_param(width, int, 0);
module_param(height, int, 0);

MODULE_AUTHOR("OZAN YANIK");
MODULE_LICENSE("GPL");

static struct fba_dev *fba_devices;

static int fba_open(struct inode *inode, struct file *filp);
static int fba_release(struct inode *inode, struct file *filp);
static ssize_t fba_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static ssize_t fba_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static loff_t fba_llseek(struct file *filp, loff_t off, int whence);
static long fba_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static struct file_operations fba_fops = {
    .owner          = THIS_MODULE,
    .open           = fba_open,
    .release        = fba_release,
    .read           = fba_read,
    .write          = fba_write,
    .llseek         = fba_llseek,
    .unlocked_ioctl = fba_ioctl,
};


static inline int viewports_intersect(
    unsigned short x1, unsigned short y1, unsigned short w1, unsigned short h1,
    unsigned short x2, unsigned short y2, unsigned short w2, unsigned short h2)
{
    if (x1 >= x2 + w2 || x2 >= x1 + w1)
        return 0;
    if (y1 >= y2 + h2 || y2 >= y1 + h1)
        return 0;
    return 1;
}

static void wake_intersecting_waiters(struct fba_dev *dev,
    unsigned short x, unsigned short y, unsigned short w, unsigned short h)
{
    struct fba_file *f;

    list_for_each_entry(f, &dev->file_list, list) {
        if (viewports_intersect(f->vp_x, f->vp_y, f->vp_width, f->vp_height,
                                x, y, w, h)) {
            f->should_wake = 1;
            wake_up_interruptible(&f->wait);
        }
    }
}

static void wake_all_waiters(struct fba_dev *dev)
{
    struct fba_file *f;

    list_for_each_entry(f, &dev->file_list, list) {
        f->should_wake = 1;
        wake_up_interruptible(&f->wait);
    }
}



static loff_t fba_llseek(struct file *filp, loff_t off, int whence)
{
    struct fba_file *fdata = filp->private_data;
    struct fba_dev *dev = fdata->dev;
    loff_t newpos;
    loff_t viewport_size;

    mutex_lock(&dev->lock);
    viewport_size = (loff_t)fdata->vp_width * fdata->vp_height;
    mutex_unlock(&dev->lock);

    switch (whence) {
    case SEEK_SET:
        newpos = off;
        break;
    case SEEK_CUR:
        newpos = filp->f_pos + off;
        break;
    case SEEK_END:
        newpos = viewport_size + off;
        break;
    default:
        return -EINVAL;
    }

    if (newpos < 0)
        return -EINVAL;
    
    filp->f_pos = newpos;
    return newpos;
}

static int fba_open(struct inode *inode, struct file *filp)
{
    struct fba_dev *dev;
    struct fba_file *fdata;

    dev = container_of(inode->i_cdev, struct fba_dev, cdev);

    fdata = kmalloc(sizeof(struct fba_file), GFP_KERNEL);
    if (!fdata)
        return -ENOMEM;

    fdata->dev = dev;
    
    mutex_lock(&dev->lock);
    fdata->vp_x = 0;
    fdata->vp_y = 0;
    fdata->vp_width = dev->width;
    fdata->vp_height = dev->height;
    mutex_unlock(&dev->lock);

    fdata->op = FB536_SET;

    init_waitqueue_head(&fdata->wait);
    fdata->should_wake = 0;

    spin_lock(&dev->file_list_lock);
    list_add(&fdata->list, &dev->file_list);
    spin_unlock(&dev->file_list_lock);

    filp->private_data = fdata;

    return 0;
}

static int fba_release(struct inode *inode, struct file *filp)
{
    struct fba_file *fdata = filp->private_data;
    struct fba_dev *dev = fdata->dev;

    spin_lock(&dev->file_list_lock);
    list_del(&fdata->list);
    spin_unlock(&dev->file_list_lock);

    kfree(fdata);

    return 0;
}

static ssize_t fba_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct fba_file *fdata = filp->private_data;
    struct fba_dev *dev = fdata->dev;
    ssize_t bytes_read = 0;
    loff_t viewport_size;
    unsigned char *result;

    mutex_lock(&dev->lock);

    if (fdata->vp_x + fdata->vp_width > dev->width || fdata->vp_y + fdata->vp_height > dev->height) 
	{
        printk(KERN_ERR "fba: Viewport buffer is outside of framebuffer\n");
        mutex_unlock(&dev->lock);
        return 0;
    }

    viewport_size = (loff_t)fdata->vp_width * fdata->vp_height;

    if (*f_pos >= viewport_size) 
	{
        printk(KERN_ERR "fba: Position out of bounds\n");
        mutex_unlock(&dev->lock);
        return 0;
    }

    if (*f_pos + count > viewport_size)
        count = viewport_size - *f_pos;

    result = kmalloc(count, GFP_KERNEL);
    if (!result) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;
    }

    while (bytes_read < count) 
    {
        loff_t vp_offset = *f_pos + bytes_read;
        unsigned int vp_row = vp_offset / fdata->vp_width;
        unsigned int vp_col = vp_offset % fdata->vp_width;
        unsigned int fb_offset = (fdata->vp_y + vp_row) * dev->width + (fdata->vp_x + vp_col);

        unsigned int bytes_left_in_row = fdata->vp_width - vp_col;
        unsigned int bytes_to_read = min((size_t)bytes_left_in_row, count - bytes_read);

        memcpy(result + bytes_read, dev->data + fb_offset, bytes_to_read);
        bytes_read += bytes_to_read;
    }

    mutex_unlock(&dev->lock);

    if (copy_to_user(buf, result, bytes_read)) 
	{
        kfree(result);
        return -EFAULT;
    }

    kfree(result);
    *f_pos += bytes_read;
    return bytes_read;
}

static inline ssize_t fba_write_with_operation(int op, struct fba_dev *dev, struct fba_file *fdata, 
                                        const char __user *buf, size_t count, loff_t *f_pos) {
    ssize_t bytes_written = 0;
    while(bytes_written < count)
    {
        loff_t vp_offset = *f_pos + bytes_written;
        unsigned int vp_row = vp_offset / fdata->vp_width;
        unsigned int vp_col = vp_offset % fdata->vp_width;
        unsigned int fb_offset = (fdata->vp_y + vp_row) * dev->width + (fdata->vp_x + vp_col);

        unsigned int bytes_left_in_row = fdata->vp_width - vp_col;
        unsigned int bytes_to_write = min((size_t)bytes_left_in_row, count - bytes_written);
        unsigned char temp_buf[256];
        unsigned int chunk = min(bytes_to_write, (unsigned int)sizeof(temp_buf));

        if (copy_from_user(temp_buf, buf + bytes_written, chunk)) {
            if(bytes_written > 0) {
                printk(KERN_ERR "fba: Could not write everything, only %zd bytes written\n", bytes_written);
                return bytes_written;
            }
            return -EFAULT;
        }

        for(int i = 0; i < chunk; i++)
        {
            unsigned int val;
            switch(op) {
                case FB536_ADD:
                    val = dev->data[fb_offset + i] + temp_buf[i];
                    dev->data[fb_offset + i] = (val > 255) ? 255 : val;
                    break;
                case FB536_SUB:
                    if (dev->data[fb_offset + i] < temp_buf[i])
                        dev->data[fb_offset + i] = 0;
                    else
                        dev->data[fb_offset + i] -= temp_buf[i];
                    break;
                case FB536_AND:
                    dev->data[fb_offset + i] = dev->data[fb_offset + i] & temp_buf[i];
                    break;
                case FB536_OR:
                    dev->data[fb_offset + i] = dev->data[fb_offset + i] | temp_buf[i];
                    break;
                case FB536_XOR:
                    dev->data[fb_offset + i] = dev->data[fb_offset + i] ^ temp_buf[i];
                    break;
            }
        }
        bytes_written += chunk;
    }
    return bytes_written;

}

// TODO: WAKE THE WAITING PROCESSES FROM THE FB536_IOCWAIT IOCTL IN THE END OF THE FUNCTION
static ssize_t fba_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
	struct fba_file *fdata = filp->private_data;
    struct fba_dev *dev = fdata->dev;
    ssize_t bytes_written = 0;
    loff_t viewport_size;

    mutex_lock(&dev->lock);

    if (fdata->vp_x + fdata->vp_width > dev->width || fdata->vp_y + fdata->vp_height > dev->height) 
	{
        mutex_unlock(&dev->lock);
        return 0;
    }

    viewport_size = (loff_t)fdata->vp_width * fdata->vp_height;

    if (*f_pos >= viewport_size) 
	{
        mutex_unlock(&dev->lock);
        return 0;
    }

    if (*f_pos + count > viewport_size)
        count = viewport_size - *f_pos;

    switch(fdata->op) {
        case FB536_SET:
            while(bytes_written < count)
            {
                loff_t vp_offset = *f_pos + bytes_written;
                unsigned int vp_row = vp_offset / fdata->vp_width;
                unsigned int vp_col = vp_offset % fdata->vp_width;
                unsigned int fb_offset = (fdata->vp_y + vp_row) * dev->width + (fdata->vp_x + vp_col);

                unsigned int bytes_left_in_row = fdata->vp_width - vp_col;
                unsigned int bytes_to_write = min((size_t)bytes_left_in_row, count - bytes_written);
                if (copy_from_user(dev->data + fb_offset, buf + bytes_written, bytes_to_write)) {
                    if(bytes_written == 0)
                        bytes_written = -EFAULT;
                    else
                        printk(KERN_ERR "fba: Could not write everything, only %zd bytes written\n", bytes_written);
                    break;
                }
                bytes_written += bytes_to_write;
            }
            break;
        case FB536_ADD:
        case FB536_SUB:
        case FB536_AND:
        case FB536_OR:
        case FB536_XOR:
            bytes_written = fba_write_with_operation(fdata->op, dev, fdata, buf, count, f_pos);
            break;
        default:
        
    }

    if (bytes_written > 0) {
        wake_intersecting_waiters(dev, fdata->vp_x, fdata->vp_y, fdata->vp_width, fdata->vp_height);
    }

    mutex_unlock(&dev->lock);

    if(bytes_written > 0)
        *f_pos += bytes_written;

    return bytes_written;

}

static long fba_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	struct fba_file *fdata = filp->private_data;
    struct fba_dev *dev = fdata->dev;
	int err = 0, ret = 0, tmp;

	if (_IOC_TYPE(cmd) != FB536_IOC_MAGIC) return -ENOTTY;
	if (_IOC_NR(cmd) > FB536_IOC_MAXNR) return -ENOTTY;

    mutex_lock(&dev->lock);

	switch(cmd) {
		case FB536_IOCRESET:{
            memset(dev->data, 0, dev->width*dev->height);
            wake_all_waiters(dev);
			break;
        }

		case FB536_IOCTSETSIZE:{
            int __user *usr_arg = (int __user *)arg;
            int new_size;
            if (copy_from_user(&new_size, usr_arg, sizeof(int))) {
                ret = -EFAULT;
                break;
            }
            int width = new_size >> 16;
            int height = new_size & 0xFFFF;
            if (width < 256 || width > 10000 || height < 256 || height > 10000) {
                printk(KERN_ERR "fba: Invalid width/height parameters\n");
                ret  = -EINVAL;
                break;
            }
            vfree(dev->data);
            dev->data = vmalloc(width * height);
            if (!dev->data) {
                ret = -ENOMEM;
                break;
            }
            dev->width = width;
            dev->height = height;
            memset(dev->data, 0, dev->width*dev->height);
            wake_all_waiters(dev);
			break;
        }

		case FB536_IOCQGETSIZE:{
            ret = (dev->width << 16) | dev->height;
            break;
        }

		case FB536_IOCSETVIEWPORT: {
            struct fb_viewport __user *usr_vp = (struct fb_viewport __user *)arg;
            struct fb_viewport new_vp;
            if(copy_from_user(&new_vp, usr_vp, sizeof(struct fb_viewport))){
                printk(KERN_ERR "fba: Could not get the user viewport buffer\n");
                ret = -EFAULT;
                break;
            }
            if (new_vp.x + new_vp.width > dev->width || new_vp.y + new_vp.height > dev->height) 
            {
                printk(KERN_ERR "fba: Viewport buffer is outside of framebuffer\n");
                ret = -EINVAL;
                break;
            }
            filp->f_pos = 0;
            fdata->vp_x = new_vp.x;
            fdata->vp_y = new_vp.y;
            fdata->vp_width = new_vp.width;
            fdata->vp_height = new_vp.height;
            fdata->should_wake = 1;
            wake_up_interruptible(&fdata->wait);
			break;
        }

		case FB536_IOCGETVIEWPORT: {
            struct fb_viewport __user *usr_vp = (struct fb_viewport __user *)arg;
            struct fb_viewport cur_vp;
            cur_vp.x = fdata->vp_x;
            cur_vp.y = fdata->vp_y;
            cur_vp.width = fdata->vp_width;
            cur_vp.height = fdata->vp_height;
            if (copy_to_user(usr_vp, &cur_vp, sizeof(struct fb_viewport))) {
                ret = -EFAULT;
                break;
            }
			break;
        }
		case FB536_IOCTSETOP:{
            if(!(filp->f_mode & (FMODE_WRITE)))
            {
                ret = -EINVAL;
                break;
            }
            if (arg > FB536_XOR) {
                ret = -EINVAL;
                break;
            }
            fdata->op = arg;
			break;
        }
		case FB536_IOCQGETOP:{
            if(!(filp->f_mode & (FMODE_WRITE)))
            {
                ret = -EINVAL;
                break;
            }
            ret = fdata->op;
			break;
        }
		case FB536_IOCWAIT:{
            if (!(filp->f_mode & FMODE_READ)) {
                ret = -EINVAL;
                break;
            }
            fdata->should_wake = 0;
            mutex_unlock(&dev->lock);
            if (wait_event_interruptible(fdata->wait, fdata->should_wake != 0)) {
                return -EINTR;
            }
            return ret;
        }
            
        default:
		    ret = -ENOTTY;
		
	}
    mutex_unlock(&dev->lock);

    return ret;
}

static int fba_setup_cdev(struct fba_dev *dev, int index)
{
    int err;
    dev_t devno = MKDEV(fba_major, index);

    cdev_init(&dev->cdev, &fba_fops);
    dev->cdev.owner = THIS_MODULE;

    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
        printk(KERN_NOTICE "fba: Error %d adding fba%d\n", err, index);

    return err;
}

static int __init fba_init(void)
{
    int result, i;
    dev_t dev;

    if (width < 256 || width > 10000 ||
        height < 256 || height > 10000) {
        printk(KERN_ERR "fba: Invalid width/height parameters\n");
        return -EINVAL;
    }

    result = alloc_chrdev_region(&dev, 0, numminors, "fb536");
    if (result < 0) {
        printk(KERN_WARNING "fba: Can't allocate major number\n");
        return result;
    }
    fba_major = MAJOR(dev);

    fba_devices = kmalloc(numminors * sizeof(struct fba_dev), GFP_KERNEL);
    if (!fba_devices) {
        result = -ENOMEM;
        goto fail_malloc_devices;
    }
    memset(fba_devices, 0, numminors * sizeof(struct fba_dev));

    for (i = 0; i < numminors; i++) {
        struct fba_dev *d = &fba_devices[i];

        d->width = width;
        d->height = height;

        d->data = vmalloc(d->width * d->height);
        if (!d->data) {
            result = -ENOMEM;
            goto fail_malloc_data;
        }
        memset(d->data, 0, d->width * d->height);

        mutex_init(&d->lock);
        spin_lock_init(&d->file_list_lock);
        INIT_LIST_HEAD(&d->file_list);

        result = fba_setup_cdev(d, i);
        if (result)
            goto fail_malloc_data;
    }

    printk(KERN_INFO "fba: Loaded with major=%d, numminors=%d, width=%d, height=%d\n",
           fba_major, numminors, width, height);

    return 0;

fail_malloc_data:
    for (i = i - 1; i >= 0; i--) {
        cdev_del(&fba_devices[i].cdev);
        if (fba_devices[i].data)
            vfree(fba_devices[i].data);
    }
    kfree(fba_devices);

fail_malloc_devices:
    unregister_chrdev_region(MKDEV(fba_major, 0), numminors);
    return result;
}

static void __exit fba_cleanup(void)
{
    int i;

    for (i = 0; i < numminors; i++) {
        cdev_del(&fba_devices[i].cdev);
        if (fba_devices[i].data)
            vfree(fba_devices[i].data);
    }

    kfree(fba_devices);
    unregister_chrdev_region(MKDEV(fba_major, 0), numminors);

    printk(KERN_INFO "fba: Unloaded\n");
}

module_init(fba_init);
module_exit(fba_cleanup);

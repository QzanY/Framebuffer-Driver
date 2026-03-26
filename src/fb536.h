#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/mutex.h>


struct fb_viewport {
	unsigned short x , y;
	unsigned short width , height ;
};

struct fba_dev {
    unsigned char *data;          /* framebuffer pixel data (width * height bytes) */
    unsigned int width;           /* current framebuffer width */
    unsigned int height;          /* current framebuffer height */
    struct mutex lock;            /* protects framebuffer data and dimensions */
    struct list_head file_list;   /* list of open file structures (for IOCWAIT) */
    spinlock_t file_list_lock;    /* protects file_list */
    struct cdev cdev;             /* character device structure */
};

struct fba_file {
    struct fba_dev *dev;        /* pointer back to device */
    unsigned short vp_x;          /* viewport x offset */
    unsigned short vp_y;          /* viewport y offset */
    unsigned short vp_width;      /* viewport width */
    unsigned short vp_height;     /* viewport height */
    int op;                       /* write operation: FB536_SET, FB536_ADD, etc. */
    wait_queue_head_t wait;       /* for FB536_IOCWAIT blocking */
    struct list_head list;        /* link into dev->file_list */
    int should_wake;
};

# define FB536_IOC_MAGIC 'F'

# define FB536_IOCRESET _IO ( FB536_IOC_MAGIC , 0)
# define FB536_IOCTSETSIZE _IO ( FB536_IOC_MAGIC , 1)
# define FB536_IOCQGETSIZE _IO ( FB536_IOC_MAGIC , 2)
# define FB536_IOCSETVIEWPORT _IOW ( FB536_IOC_MAGIC , 3, struct fb_viewport )
# define FB536_IOCGETVIEWPORT _IOR ( FB536_IOC_MAGIC , 4, struct fb_viewport )
# define FB536_IOCTSETOP _IO ( FB536_IOC_MAGIC , 5)
# define FB536_IOCQGETOP _IO ( FB536_IOC_MAGIC , 6)
# define FB536_IOCWAIT _IO ( FB536_IOC_MAGIC , 7)
# define FB536_IOC_MAXNR 7



#define FB536_SET 0
#define FB536_ADD 1
#define FB536_SUB 2
#define FB536_AND 3
#define FB536_OR 4
#define FB536_XOR 5

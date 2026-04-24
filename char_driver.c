/*
 * char_driver.c - Complete Linux Character Device Driver Demo
 *
 * Author: Veeresh
 *
 * This module demonstrates:
 * - Dynamic device number allocation
 * - Character device registration
 * - Read/Write operations with user-space data transfer
 * - IOCTL (Input/Output Control) commands
 * - MMAP (Memory Mapping) for zero-copy data access
 * - Mutex synchronization for thread safety
 * - Spinlock synchronization for atomic operations
 * - Interrupt handling (simulated)
 * - Tasklet for deferred work (bottom half - atomic context)
 * - Workqueue for deferred work (bottom half - process context)
 * - Blocking I/O using wait queues
 * - Poll/select/epoll support
 * - Dynamic memory allocation with kmalloc/kfree
 * - DMA buffer allocation using __get_free_pages + virt_to_phys
 * - Structured debug logging with proper log levels
 * - Proper cleanup on module exit
 */

/* ============================================================================
 * DEBUGGING INFRASTRUCTURE — must come before ALL #includes
 * ============================================================================
 *
 * WHY pr_fmt?
 * ===========
 * Defining pr_fmt() prepends "CharDev: " to every pr_*() call in this
 * file automatically.  Benefits:
 *
 *   1. CONSISTENCY — impossible to forget or misspell the prefix.
 *   2. GREP-ABILITY — filter the entire driver with one command:
 *        sudo dmesg | grep "CharDev:"
 *   3. ZERO OVERHEAD — resolved at compile time, no runtime cost.
 *
 * pr_fmt MUST be #defined before any #include that pulls in
 * <linux/printk.h> (done transitively by <linux/kernel.h>), so it
 * must appear at the very top of the file.
 *
 * WHY pr_*() INSTEAD OF BARE printk()?
 * ======================================
 * pr_info(), pr_err(), pr_warn(), pr_debug() etc. are the modern
 * preferred wrappers.  They automatically insert the KERN_* level
 * without you having to type it by hand.
 *
 *   Old style:  printk(KERN_INFO "CharDev: msg\n");
 *   New style:  pr_info("msg\n");           <- same output, cleaner
 *
 * LOG LEVEL GUIDE — which level to use where:
 * ============================================
 *   pr_alert  — failure that prevents the module from loading
 *   pr_err    — per-operation failure returned to caller (-EFAULT, etc.)
 *   pr_warn   — unexpected but handled (bad ioctl magic, truncation)
 *   pr_notice — module lifecycle events (load / unload summary)
 *   pr_info   — normal per-operation events (entry, byte counts, addresses)
 *   pr_debug  — high-frequency or very verbose detail (poll, tasklet,
 *               workqueue, ISR rejects).  Invisible by default; enable
 *               at runtime with dynamic_debug:
 *                 echo "file char_driver.c +p" > \
 *                   /sys/kernel/debug/dynamic_debug/control
 */
#define pr_fmt(fmt) "CharDev: " fmt

#include <linux/module.h>       /* Core header for loading LKMs */
#include <linux/kernel.h>       /* Contains types, macros, functions for kernel */
#include <linux/init.h>         /* __init / __exit macros */
#include <linux/fs.h>           /* File system support */
#include <linux/cdev.h>         /* Character device structures */
#include <linux/device.h>       /* Kernel Driver Model */
#include <linux/sched.h>        /* task_struct, current macro */
#include <linux/mutex.h>        /* Mutex synchronization */
#include <linux/spinlock.h>     /* Spinlock synchronization */
#include <linux/uaccess.h>      /* copy_to_user / copy_from_user */
#include <linux/interrupt.h>    /* IRQ handling, tasklets, irqreturn_t */
#include <linux/workqueue.h>    /* Workqueue (deferred work) */
#include <linux/poll.h>         /* poll / select support */
#include <linux/delay.h>        /* msleep, udelay, mdelay */
#include <linux/slab.h>         /* kmalloc, kfree, kzalloc */
#include <linux/ioctl.h>        /* ioctl definitions */
#include <linux/mm.h>           /* Memory management, mmap */
#include <linux/vmalloc.h>      /* vmalloc */
#include <asm/io.h>             /* virt_to_phys, phys_to_virt */
#include <linux/dma-mapping.h>  /* dma_addr_t and DMA helpers */

/* ============================================================================
 * MODULE METADATA
 * ============================================================================ */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Veeresh");
MODULE_DESCRIPTION("Complete char driver: sync, IRQ, deferred work, "
                   "blocking I/O, poll, ioctl, mmap, kmalloc, DMA, debug");
MODULE_VERSION("13.0");

/* ============================================================================
 * CONSTANTS
 * ============================================================================ */

#define DEVICE_NAME     "mychardev"  /* Appears in /proc/devices and /dev/ */
#define NUM_DEVICES     1            /* Number of minor devices             */
#define BUFFER_SIZE     1024         /* Read/write buffer size in bytes     */

/*
 * SIMULATED_IRQ — we share the keyboard IRQ (1) for demonstration.
 * A real driver uses the IRQ provided by its hardware resource.
 */
#define SIMULATED_IRQ   1

/*
 * DMA_BUFFER_SIZE — one page (4 096 bytes on x86).
 * Real sizes: Ethernet descriptor ~256 B, USB bulk ~512 B, PCIe 4–64 KB.
 */
#define DMA_BUFFER_SIZE PAGE_SIZE

/* ============================================================================
 * IOCTL DEFINITIONS
 * ============================================================================
 *
 * Magic 'M' (for "MyCharDev") tags commands as belonging to this driver.
 * _IOC_TYPE(cmd) extracts the magic; mismatches are rejected early.
 *
 * _IOW → userspace Writes into the driver  (SET)
 * _IOR → userspace Reads  from the driver  (GET)
 */
#define IOCTL_MAGIC 'M'

struct ioctl_data {
    char   buffer[BUFFER_SIZE]; /* Data payload                     */
    size_t size;                /* Valid bytes in payload            */
};

#define SET_BUFFER_VALUE  _IOW(IOCTL_MAGIC, 1, struct ioctl_data)
#define GET_BUFFER_VALUE  _IOR(IOCTL_MAGIC, 2, struct ioctl_data)

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================ */

static dev_t              dev_number;          /* Major + minor numbers   */
static struct cdev        my_cdev;             /* Kernel cdev object      */
static struct class      *my_class = NULL;     /* /sys class entry        */

/*
 * device_mutex protects device_buffer and buffer_size from concurrent
 * access by multiple processes.  Must NOT be held in interrupt context.
 */
static DEFINE_MUTEX(device_mutex);

/*
 * device_spinlock protects state touched inside the ISR (irq_count,
 * buffer_size, data_available).  Spinlocks are safe across
 * interrupt/process context; mutexes are not (they can sleep).
 */
static spinlock_t device_spinlock;

/* Read/write buffer — allocated in init, freed in exit */
static char  *device_buffer = NULL;
static size_t buffer_size   = 0;

/* MMAP buffer — one page, mapped into userspace on demand */
static unsigned long mmap_buffer = 0;
#define MMAP_SIZE PAGE_SIZE

/*
 * DMA buffer
 * ----------
 * dma_buffer_virt  CPU-side virtual address (driver uses this)
 * dma_buffer_phys  Physical / bus address   (hardware DMA engine needs this)
 *
 * We use __get_free_pages() + virt_to_phys() instead of
 * dma_alloc_coherent(NULL, ...) to avoid a NULL-deref crash on
 * Linux >= 5.0.  In a real PCI/platform driver use
 * dma_alloc_coherent(&pdev->dev, ...) with a valid device pointer.
 */
static unsigned long dma_buffer_virt = 0;
static dma_addr_t    dma_buffer_phys = 0;

/* Event counters — logged on module unload to summarise activity */
static unsigned long irq_count     = 0;
static unsigned long tasklet_count = 0;
static unsigned long work_count    = 0;

/* Tasklet and workqueue objects */
static struct tasklet_struct     my_tasklet;
static struct workqueue_struct  *my_workqueue;
static struct work_struct        my_work;

/* Wait queue — readers block here until data_available becomes true */
static DECLARE_WAIT_QUEUE_HEAD(read_wait_queue);
static int data_available = 0;

/* ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================ */
static int         device_open(struct inode *, struct file *);
static int         device_release(struct inode *, struct file *);
static ssize_t     device_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t     device_write(struct file *, const char __user *, size_t, loff_t *);
static __poll_t    device_poll(struct file *, struct poll_table_struct *);
static long        device_ioctl(struct file *, unsigned int, unsigned long);
static int         device_mmap(struct file *, struct vm_area_struct *);
static irqreturn_t device_irq_handler(int irq, void *dev_id);
static void        device_tasklet_handler(unsigned long data);
static void        device_work_handler(struct work_struct *work);

/* ============================================================================
 * FILE OPERATIONS TABLE
 * ============================================================================ */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = device_open,
    .release        = device_release,
    .read           = device_read,
    .write          = device_write,
    .poll           = device_poll,
    .unlocked_ioctl = device_ioctl,
    .mmap           = device_mmap,
};

/* ============================================================================
 * OPEN / RELEASE
 * ============================================================================ */

/*
 * device_open() — called each time a process opens /dev/mychardev.
 *
 * LOG LEVEL: pr_info
 *   Opening the device is a normal, expected operation.
 *   Logging PID and process name lets you see which userspace program
 *   is talking to the driver without needing strace.
 *
 * WHY LOG HERE:
 *   Lets you correlate userspace open() calls with kernel activity.
 *   Helps catch double-open bugs or unexpected openers (e.g. udev).
 */
static int device_open(struct inode *inode, struct file *file)
{
    pr_info("[OPEN] /dev/%s opened by PID %d (%s)\n",
            DEVICE_NAME, current->pid, current->comm);
    return 0;
}

/*
 * device_release() — called when the last file descriptor to the device
 * is closed (reference count drops to zero).
 *
 * LOG LEVEL: pr_info
 *   Symmetric with device_open() — always log both sides of the
 *   open/close lifecycle to detect file-descriptor leaks.
 *
 * WHY LOG HERE:
 *   If you see an open without a matching release in dmesg, the
 *   userspace program is leaking a file descriptor.
 */
static int device_release(struct inode *inode, struct file *file)
{
    pr_info("[RELEASE] /dev/%s closed by PID %d (%s)\n",
            DEVICE_NAME, current->pid, current->comm);
    return 0;
}

/* ============================================================================
 * READ
 * ============================================================================ */

/*
 * device_read() — copy data from the kernel buffer to userspace.
 *
 * LOG LEVELS:
 *   pr_info  — entry (PID, requested bytes, offset) and success (bytes, new offset)
 *   pr_debug — EOF condition (high-frequency; silent by default)
 *   pr_err   — copy_to_user() failure (bad user pointer)
 */
static ssize_t device_read(struct file *file, char __user *buf,
                            size_t len, loff_t *off)
{
    size_t bytes_to_read;
    int    ret;

    /*
     * WHY LOG ENTRY WITH PID, BYTES REQUESTED, AND OFFSET:
     * Shows which process is reading, how many bytes it wants, and
     * the current file position.  Critical when debugging multi-read
     * sequences (e.g. cat reads in 4 096-byte chunks until EOF).
     */
    pr_info("[READ] Entry — PID %d (%s) requests %zu bytes at offset %lld\n",
            current->pid, current->comm, len, *off);

    mutex_lock(&device_mutex);

    if (*off >= buffer_size) {
        /*
         * WHY pr_debug FOR EOF:
         * EOF is hit on every read() loop — cat, shell redirects, etc.
         * At pr_info this would flood dmesg with identical lines.
         * pr_debug lets developers opt in only when tracing read loops.
         */
        pr_debug("[READ] EOF — offset %lld >= buffer_size %zu\n",
                 *off, buffer_size);
        mutex_unlock(&device_mutex);
        return 0;
    }

    bytes_to_read = buffer_size - *off;
    if (bytes_to_read > len)
        bytes_to_read = len;

    if (bytes_to_read == 0) {
        mutex_unlock(&device_mutex);
        return 0;
    }

    /*
     * WHY LOG ERRORS FROM copy_to_user():
     * A non-zero return means the userspace buffer pointer is bad
     * (invalid, unmapped, or read-only).  This is almost always a
     * programming error in the caller.  pr_err() makes it stand out.
     */
    ret = copy_to_user(buf, device_buffer + *off, bytes_to_read);
    if (ret != 0) {
        pr_err("[READ] copy_to_user() failed — %d bytes not copied "
               "(bad user pointer 0x%px?)\n", ret, buf);
        mutex_unlock(&device_mutex);
        return -EFAULT;
    }

    *off += bytes_to_read;

    /*
     * WHY LOG BYTE COUNT AND NEW OFFSET:
     * Byte count confirms the transfer happened.
     * New offset verifies that a subsequent read will resume from
     * the correct position — critical for multi-chunk reads.
     */
    pr_info("[READ] Success — %zu bytes copied to PID %d, new offset %lld\n",
            bytes_to_read, current->pid, *off);

    mutex_unlock(&device_mutex);
    return bytes_to_read;
}

/* ============================================================================
 * WRITE
 * ============================================================================ */

/*
 * device_write() — copy data from userspace into the kernel buffer.
 *
 * LOG LEVELS:
 *   pr_info  — entry (PID, requested bytes) and success (bytes stored)
 *   pr_warn  — write exceeds buffer capacity (data silently truncated)
 *   pr_err   — copy_from_user() failure (bad user pointer)
 */
static ssize_t device_write(struct file *file, const char __user *buf,
                             size_t len, loff_t *off)
{
    size_t        bytes_to_write;
    int           ret;
    unsigned long flags;

    /*
     * WHY LOG ENTRY WITH REQUESTED LENGTH:
     * Tells you immediately if userspace is sending more data than
     * expected.  Also confirms which process is the writer.
     */
    pr_info("[WRITE] Entry — PID %d (%s) wants to write %zu bytes\n",
            current->pid, current->comm, len);

    bytes_to_write = (len < BUFFER_SIZE) ? len : BUFFER_SIZE;

    if (bytes_to_write < len) {
        /*
         * WHY pr_warn FOR TRUNCATION:
         * Silently discarding data is an easy-to-miss bug.
         * A warning makes it immediately visible in dmesg without
         * being as alarming as pr_err (we do handle it gracefully).
         */
        pr_warn("[WRITE] Requested %zu bytes > buffer %d bytes — "
                "truncating to %zu bytes\n", len, BUFFER_SIZE, bytes_to_write);
    }

    /*
     * WHY LOG ERRORS FROM copy_from_user():
     * A non-zero return means the userspace buffer pointer is invalid.
     * This is a programming error in the caller; pr_err() surfaces it.
     */
    ret = copy_from_user(device_buffer, buf, bytes_to_write);
    if (ret != 0) {
        pr_err("[WRITE] copy_from_user() failed — %d bytes not copied "
               "(bad user pointer 0x%px?)\n", ret, buf);
        return -EFAULT;
    }

    /*
     * Use spinlock (not mutex) to update buffer_size because the ISR
     * also reads this value.  Spinlocks are safe in interrupt context;
     * mutexes are not (they can sleep).
     */
    spin_lock_irqsave(&device_spinlock, flags);
    buffer_size = bytes_to_write;
    spin_unlock_irqrestore(&device_spinlock, flags);

    /*
     * WHY LOG SUCCESS WITH EXACT BYTE COUNT:
     * Confirms how many bytes actually landed in the kernel buffer.
     * Essential for debugging protocol mismatches (e.g. user wrote
     * 1 025 bytes but only 1 024 were accepted due to truncation).
     */
    pr_info("[WRITE] Success — %zu bytes stored in kernel buffer by PID %d\n",
            bytes_to_write, current->pid);

    return bytes_to_write;
}

/* ============================================================================
 * POLL
 * ============================================================================ */

/*
 * device_poll() — implements select() / poll() / epoll() support.
 *
 * LOG LEVEL: pr_debug
 *   poll() is called at very high frequency by event loops (libuv,
 *   epoll, etc.).  At pr_info it would flood dmesg and make the log
 *   useless.  pr_debug is invisible by default — enable only when
 *   debugging the poll implementation itself.
 */
static __poll_t device_poll(struct file *file, struct poll_table_struct *wait)
{
    __poll_t mask = 0;

    /*
     * WHY pr_debug HERE:
     * An event-driven application can call poll() thousands of times
     * per second.  Keeping this at debug level prevents log flooding.
     */
    pr_debug("[POLL] Called by PID %d — data_available=%d\n",
             current->pid, data_available);

    poll_wait(file, &read_wait_queue, wait);

    if (data_available) {
        mask |= POLLIN | POLLRDNORM;
        pr_debug("[POLL] Reporting POLLIN to PID %d\n", current->pid);
    }

    mask |= POLLOUT | POLLWRNORM; /* Always writable */
    return mask;
}

/* ============================================================================
 * IOCTL
 * ============================================================================ */

/*
 * device_ioctl() — handle configuration and control commands from userspace.
 *
 * LOG LEVELS:
 *   pr_info  — command dispatch and success result
 *   pr_warn  — bad magic number or unknown command (caller bug)
 *   pr_err   — kmalloc failure or copy_from/to_user failure
 *
 * WHY LOG THE RAW COMMAND VALUE AND ITS DECODED FIELDS:
 *   The ioctl cmd encodes direction, size, magic, and index in one
 *   32-bit value.  Printing all fields lets you immediately spot a
 *   mismatch between the driver and the application (e.g. struct size
 *   changed, wrong magic, wrong command number) without running a
 *   debugger or manually decoding the bitmask.
 */
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct ioctl_data *data;
    int    ret       = 0;
    size_t copy_size;

    pr_info("[IOCTL] ============ BEGIN ============\n");

    /*
     * WHY LOG ALL DECODED CMD FIELDS:
     *   type  — magic char, must be 'M' = 0x4D
     *   nr    — command index: 1=SET, 2=GET
     *   size  — sizeof(struct ioctl_data) encoded at compile time
     *   dir   — 0=none, 1=write-to-kernel, 2=read-from-kernel
     * A mismatch in any field instantly points to a version skew or
     * wrong fd being passed from userspace.
     */
    pr_info("[IOCTL] PID %d (%s) — cmd=0x%08x  "
            "type='%c'(0x%x)  nr=%u  size=%u  dir=%u\n",
            current->pid, current->comm,
            cmd,
            _IOC_TYPE(cmd), _IOC_TYPE(cmd),
            _IOC_NR(cmd),
            _IOC_SIZE(cmd),
            _IOC_DIR(cmd));

    data = kmalloc(sizeof(struct ioctl_data), GFP_KERNEL);
    if (!data) {
        /*
         * WHY pr_err FOR KMALLOC FAILURE:
         * OOM during ioctl handling is abnormal and the caller will
         * receive -ENOMEM.  pr_err ensures the failure is visible
         * in dmesg for post-mortem analysis.
         */
        pr_err("[IOCTL] kmalloc(%zu) failed — system OOM?\n",
               sizeof(struct ioctl_data));
        return -ENOMEM;
    }

    /*
     * WHY VALIDATE MAGIC BEFORE ACTING ON THE COMMAND:
     * A wrong magic usually means the caller opened the wrong device
     * file.  Rejecting early (with a clear warning) prevents
     * misinterpreting a different driver's command as ours.
     *
     * WHY pr_warn (NOT pr_err):
     * This is a caller bug, not a driver bug.  pr_warn is visible but
     * not as alarming as pr_err which implies a driver-internal fault.
     */
    if (_IOC_TYPE(cmd) != IOCTL_MAGIC) {
        pr_warn("[IOCTL] Bad magic 0x%x (expected '%c'=0x%x) — "
                "command not intended for this driver\n",
                _IOC_TYPE(cmd), IOCTL_MAGIC, IOCTL_MAGIC);
        kfree(data);
        return -ENOTTY;
    }

    switch (cmd) {

    /* ---------------------------------------------------------------
     * SET_BUFFER_VALUE — userspace writes data into the kernel buffer
     * --------------------------------------------------------------- */
    case SET_BUFFER_VALUE:
        pr_info("[IOCTL] Dispatching SET_BUFFER_VALUE\n");

        /*
         * WHY LOG BEFORE copy_from_user():
         * If copy_from_user() oopses on a bad pointer, the log entry
         * just before it tells you the crash happened here, not
         * somewhere else in the driver.
         */
        pr_info("[IOCTL] Copying %zu bytes of ioctl_data from user "
                "address 0x%lx...\n", sizeof(struct ioctl_data), arg);

        if (copy_from_user(data, (struct ioctl_data __user *)arg,
                           sizeof(struct ioctl_data))) {
            pr_err("[IOCTL] copy_from_user() failed for SET — "
                   "invalid user address 0x%lx\n", arg);
            kfree(data);
            return -EFAULT;
        }

        pr_info("[IOCTL] User payload: %zu bytes requested\n", data->size);

        if (data->size > BUFFER_SIZE) {
            pr_warn("[IOCTL] Payload size %zu > BUFFER_SIZE %d — "
                    "clamping to fit\n", data->size, BUFFER_SIZE);
            data->size = BUFFER_SIZE;
        }

        mutex_lock(&device_mutex);
        memcpy(device_buffer, data->buffer, data->size);
        buffer_size = data->size;
        mutex_unlock(&device_mutex);

        /*
         * WHY LOG A DATA PREVIEW:
         * Printing the first ~40 chars confirms the correct string
         * landed in the buffer without needing a separate test program.
         * %.40s prevents log spam for large payloads.
         */
        pr_info("[IOCTL] SET_BUFFER_VALUE OK — %zu bytes stored, "
                "preview: \"%.40s\"\n",
                data->size, device_buffer);
        ret = 0;
        break;

    /* ---------------------------------------------------------------
     * GET_BUFFER_VALUE — kernel returns current buffer to userspace
     * --------------------------------------------------------------- */
    case GET_BUFFER_VALUE:
        pr_info("[IOCTL] Dispatching GET_BUFFER_VALUE\n");

        mutex_lock(&device_mutex);
        memset(data, 0, sizeof(struct ioctl_data));
        copy_size = (buffer_size < BUFFER_SIZE) ? buffer_size : BUFFER_SIZE;
        memcpy(data->buffer, device_buffer, copy_size);
        data->size = copy_size;
        mutex_unlock(&device_mutex);

        pr_info("[IOCTL] Copying %zu bytes to user address 0x%lx...\n",
                data->size, arg);

        if (copy_to_user((struct ioctl_data __user *)arg, data,
                         sizeof(struct ioctl_data))) {
            pr_err("[IOCTL] copy_to_user() failed for GET — "
                   "invalid user address 0x%lx\n", arg);
            kfree(data);
            return -EFAULT;
        }

        pr_info("[IOCTL] GET_BUFFER_VALUE OK — %zu bytes returned to "
                "PID %d\n", data->size, current->pid);
        ret = 0;
        break;

    /* ---------------------------------------------------------------
     * Unknown — magic matched but command number unrecognised
     * --------------------------------------------------------------- */
    default:
        /*
         * WHY pr_warn (NOT pr_err):
         * The magic was correct so the caller is talking to the right
         * driver, but asked for an unimplemented command.  Could be a
         * version mismatch or future extension.  Warn, not error.
         */
        pr_warn("[IOCTL] Unknown command nr=%u (raw=0x%08x) — "
                "valid: SET=0x%lx GET=0x%lx\n",
                _IOC_NR(cmd), cmd,
                (unsigned long)SET_BUFFER_VALUE,
                (unsigned long)GET_BUFFER_VALUE);
        ret = -ENOTTY;
        break;
    }

    kfree(data);

    /*
     * WHY LOG THE RETURN VALUE:
     * Makes it trivial to confirm in dmesg that an ioctl succeeded
     * (ret=0) or failed (ret=-EFAULT etc.) without attaching a debugger.
     */
    pr_info("[IOCTL] Complete — return %d\n", ret);
    pr_info("[IOCTL] ============ END ==============\n");
    return ret;
}

/* ============================================================================
 * MMAP
 * ============================================================================ */

/*
 * device_mmap() — map the kernel mmap_buffer into userspace.
 *
 * LOG LEVELS:
 *   pr_info  — entry, both addresses, success
 *   pr_warn  — mapping request larger than our buffer
 *   pr_err   — remap_pfn_range() failure
 */
static int device_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size;
    unsigned long pfn;

    pr_info("[MMAP] ============ BEGIN ============\n");
    pr_info("[MMAP] Called by PID %d (%s)\n",
            current->pid, current->comm);

    size = vma->vm_end - vma->vm_start;

    pr_info("[MMAP] Request: %lu bytes (%lu pages) at userspace vaddr 0x%lx\n",
            size, size / PAGE_SIZE, vma->vm_start);
    pr_info("[MMAP] Buffer available: %lu bytes\n", MMAP_SIZE);

    if (size > MMAP_SIZE) {
        pr_warn("[MMAP] Requested %lu bytes exceeds buffer %lu bytes — "
                "rejecting\n", size, MMAP_SIZE);
        return -EINVAL;
    }

    vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
    pfn = virt_to_phys((void *)mmap_buffer) >> PAGE_SHIFT;

    pr_info("[MMAP] Kernel virt: 0x%lx  phys: 0x%llx  PFN: 0x%lx\n",
            mmap_buffer,
            (unsigned long long)virt_to_phys((void *)mmap_buffer),
            pfn);

    if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
        pr_err("[MMAP] remap_pfn_range() failed — could not map kernel "
               "buffer to userspace\n");
        return -EAGAIN;
    }

    pr_info("[MMAP] Success — %lu bytes mapped to userspace vaddr 0x%lx\n",
            size, vma->vm_start);
    pr_info("[MMAP] Zero-copy access now active\n");
    pr_info("[MMAP] ============ END ==============\n");
    return 0;
}

/* ============================================================================
 * WORKQUEUE HANDLER (process context — can sleep)
 * ============================================================================ */

/*
 * device_work_handler() — runs in a kernel thread, scheduled by the tasklet.
 *
 * LOG LEVEL: pr_debug
 *   Workqueue items fire for every accepted interrupt (every 10th IRQ).
 *   On an active keyboard this would produce a log line per keypress.
 *   pr_debug keeps it silent by default; enable with dynamic_debug when
 *   specifically tracing deferred-work execution paths.
 *
 * WHY pr_debug HERE:
 *   High frequency + low information value per line = debug level.
 *   Use pr_info only for events that are rare or carry diagnostic value.
 */
static void device_work_handler(struct work_struct *work)
{
    pr_debug("[WORKQUEUE] Started — work_count before increment: %lu\n",
             work_count);

    mutex_lock(&device_mutex);
    work_count++;
    msleep(10); /* Safe: process context allows sleeping */
    mutex_unlock(&device_mutex);

    pr_debug("[WORKQUEUE] Complete — work_count now: %lu\n", work_count);
}

/* ============================================================================
 * TASKLET HANDLER (atomic / softirq context — must NOT sleep)
 * ============================================================================ */

/*
 * device_tasklet_handler() — bottom-half, runs in softirq context.
 * Must not sleep; must not acquire mutexes.
 *
 * LOG LEVEL: pr_debug
 *   Same reasoning as workqueue — fires too frequently to log at pr_info.
 *
 * WHY pr_debug IN ATOMIC CONTEXT:
 *   printk() IS safe in atomic context (it uses its own internal
 *   spinlock).  However it adds latency.  Using pr_debug ensures that
 *   in production (where dynamic debug is off) this path adds zero
 *   logging overhead to the softirq path.
 */
static void device_tasklet_handler(unsigned long data)
{
    unsigned long flags;

    pr_debug("[TASKLET] Started — tasklet_count before: %lu\n",
             tasklet_count);

    spin_lock_irqsave(&device_spinlock, flags);
    tasklet_count++;
    spin_unlock_irqrestore(&device_spinlock, flags);

    queue_work(my_workqueue, &my_work); /* Offload sleeping work */

    pr_debug("[TASKLET] Complete — tasklet_count now: %lu\n", tasklet_count);
}

/* ============================================================================
 * INTERRUPT SERVICE ROUTINE
 * ============================================================================
 *
 * device_irq_handler() — hard-IRQ context; MUST NOT sleep; spinlocks only.
 *
 * We share IRQ 1 (keyboard) and process every 10th interrupt to keep
 * log output manageable.  A real driver fires only for its own hardware.
 *
 * LOG LEVELS:
 *   pr_debug — rejected interrupts (9 out of every 10 keyboard IRQs)
 *   pr_info  — accepted interrupts (the 1 in 10 we process)
 *
 * WHY CAREFUL LOG LEVELS IN THE ISR:
 *   printk() inside an ISR acquires a console spinlock, adding
 *   microseconds to IRQ latency.  pr_debug compiles to a no-op when
 *   dynamic debug is disabled, meaning zero overhead in production for
 *   the hot (rejected) path.  pr_info on the cold (accepted) path is
 *   acceptable because it fires infrequently.
 */
static irqreturn_t device_irq_handler(int irq, void *dev_id)
{
    unsigned long flags;
    static int irq_counter = 0;

    irq_counter++;

    if (irq_counter % 10 != 0) {
        /*
         * WHY pr_debug FOR REJECTED INTERRUPTS:
         * Sharing IRQ 1 means we see ~2 interrupts per keypress.
         * At pr_info, normal typing would generate ~180 log lines
         * per minute — completely unusable.  pr_debug lets the
         * developer opt in only when actively debugging the ISR.
         */
        pr_debug("[ISR] IRQ %d received (irq_counter=%d) — "
                 "not our turn, returning IRQ_NONE\n", irq, irq_counter);
        return IRQ_NONE;
    }

    /*
     * WHY pr_info FOR ACCEPTED INTERRUPTS:
     * Accepted interrupts drive the data-available / wake-up / tasklet
     * pipeline — the core of the interrupt handling logic.  Logging at
     * pr_info gives visibility into this flow without being noisy
     * (fires only once per 10 keyboard interrupts).
     */
    pr_info("[ISR] IRQ %d accepted (irq_counter=%d) — handling\n",
            irq, irq_counter);

    spin_lock_irqsave(&device_spinlock, flags);
    irq_count++;
    data_available = 1;
    spin_unlock_irqrestore(&device_spinlock, flags);

    /*
     * WHY LOG WAKE-UP AND TASKLET SCHEDULE SEPARATELY:
     * These two side-effects are the ISR's primary jobs.
     * Seeing both in dmesg confirms the full pipeline was triggered,
     * which makes debugging blocked readers and missing tasklets easy.
     */
    pr_info("[ISR] Waking read_wait_queue (total irq_count=%lu)\n",
            irq_count);
    wake_up_interruptible(&read_wait_queue);

    pr_info("[ISR] Scheduling tasklet\n");
    tasklet_schedule(&my_tasklet);

    return IRQ_HANDLED;
}

/* ============================================================================
 * MODULE INIT
 * ============================================================================
 *
 * LOG LEVELS:
 *   pr_notice — module lifecycle banners (always visible, very low volume)
 *   pr_info   — per-step progress, allocated addresses
 *   pr_alert  — any failure that prevents the module from loading
 *
 * WHY LOG EVERY INIT STEP:
 *   Module init runs once.  Each step can fail independently.
 *   Granular logs let you pinpoint from a single dmesg exactly which
 *   step failed without re-running or attaching a debugger.
 *   The step counter (Step N/9) shows at a glance how far init got.
 *
 * WHY pr_notice FOR LIFECYCLE BANNERS:
 *   pr_notice sits one level above pr_info.  On systems with a high
 *   default log level, pr_info messages can be suppressed while
 *   pr_notice still shows.  The load/unload banners should ALWAYS
 *   be visible in dmesg regardless of the system's log threshold.
 */
static int __init chardev_init(void)
{
    int ret;

    pr_notice("========================================\n");
    pr_notice("Loading %s driver v13.0\n", DEVICE_NAME);
    pr_notice("Author: Veeresh\n");
    pr_notice("========================================\n");

    /* ------------------------------------------------------------------
     * Step 1: Read/write buffer
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 1/9 — Allocating read/write buffer (%d bytes)\n",
            BUFFER_SIZE);

    device_buffer = kmalloc(BUFFER_SIZE, GFP_KERNEL);
    if (!device_buffer) {
        /*
         * WHY pr_alert FOR INIT FAILURES:
         * A failure here means the module cannot load at all.
         * pr_alert is visible even when the console log level is
         * set very low.  It signals "immediate attention required"
         * without asserting a full kernel panic.
         */
        pr_alert("[INIT] FAILED step 1 — kmalloc(%d) returned NULL\n",
                 BUFFER_SIZE);
        return -ENOMEM;
    }
    memset(device_buffer, 0, BUFFER_SIZE);
    pr_info("[INIT] Read/write buffer allocated at %p (%d bytes)\n",
            device_buffer, BUFFER_SIZE);

    /* ------------------------------------------------------------------
     * Step 2: MMAP buffer
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 2/9 — Allocating mmap buffer (%lu bytes)\n",
            MMAP_SIZE);

    mmap_buffer = __get_free_pages(GFP_KERNEL, 0);
    if (!mmap_buffer) {
        pr_alert("[INIT] FAILED step 2 — __get_free_pages() for mmap "
                 "returned 0\n");
        kfree(device_buffer);
        return -ENOMEM;
    }
    memset((void *)mmap_buffer, 0, MMAP_SIZE);
    snprintf((char *)mmap_buffer, MMAP_SIZE,
             "MMAP buffer initialized. PAGE_SIZE=%lu bytes", PAGE_SIZE);

    /*
     * WHY LOG BOTH VIRTUAL AND PHYSICAL ADDRESSES:
     * Virtual address  — what driver code uses for reads/writes.
     * Physical address — what you would verify in /proc/iomem or pass
     *                    to hardware.  Confirms the allocator returned
     *                    a physically contiguous, page-aligned region.
     */
    pr_info("[INIT] MMAP buffer — virt: 0x%lx  phys: 0x%llx  size: %lu B\n",
            mmap_buffer,
            (unsigned long long)virt_to_phys((void *)mmap_buffer),
            MMAP_SIZE);

    /* ------------------------------------------------------------------
     * Step 3: DMA buffer
     *
     * WHY LOG DMA ALLOCATION IN DETAIL:
     *   The physical address is the value you would write into a real
     *   hardware DMA address register.  Logging it here means you can
     *   verify (via /proc/iomem or a hardware debugger) that the
     *   correct physical page was allocated without a separate tool.
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 3/9 — Allocating DMA buffer (%lu bytes)\n",
            (unsigned long)DMA_BUFFER_SIZE);
    pr_info("[INIT] NOTE: Using __get_free_pages() + virt_to_phys() "
            "instead of dma_alloc_coherent(NULL,...) to avoid NULL-deref "
            "crash on kernel >= 5.0\n");

    dma_buffer_virt = __get_free_pages(GFP_KERNEL, 0);
    if (!dma_buffer_virt) {
        pr_alert("[INIT] FAILED step 3 — __get_free_pages() for DMA "
                 "buffer returned 0\n");
        free_pages(mmap_buffer, 0);
        kfree(device_buffer);
        return -ENOMEM;
    }

    dma_buffer_phys = (dma_addr_t)virt_to_phys((void *)dma_buffer_virt);
    SetPageReserved(virt_to_page((void *)dma_buffer_virt));

    memset((void *)dma_buffer_virt, 0, DMA_BUFFER_SIZE);
    snprintf((char *)dma_buffer_virt, DMA_BUFFER_SIZE,
             "DMA buffer initialized. Size=%lu bytes",
             (unsigned long)DMA_BUFFER_SIZE);

    /*
     * WHY LOG VIRTUAL, PHYSICAL, AND CONTENT:
     *   virt  = what the driver uses for memcpy / string ops.
     *   phys  = what a real DMA engine register would need.
     *   content preview = confirms init without a separate userspace test.
     *
     * In a real PCI driver you would do:
     *   iowrite32(lower_32_bits(dma_buffer_phys), bar + DMA_ADDR_LO);
     *   iowrite32(upper_32_bits(dma_buffer_phys), bar + DMA_ADDR_HI);
     */
    pr_info("[INIT] DMA buffer — virt: 0x%lx  phys: 0x%llx  size: %lu B\n",
            dma_buffer_virt,
            (unsigned long long)dma_buffer_phys,
            (unsigned long)DMA_BUFFER_SIZE);
    pr_info("[INIT] DMA buffer content: \"%.60s\"\n",
            (char *)dma_buffer_virt);
    pr_info("[INIT] In a real driver phys=0x%llx would be written into "
            "the hardware DMA address register\n",
            (unsigned long long)dma_buffer_phys);

    /* ------------------------------------------------------------------
     * Step 4: Spinlock
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 4/9 — Initialising spinlock\n");
    spin_lock_init(&device_spinlock);
    pr_info("[INIT] Spinlock ready\n");

    /* ------------------------------------------------------------------
     * Step 5: Tasklet
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 5/9 — Initialising tasklet\n");
    tasklet_init(&my_tasklet, device_tasklet_handler, 0);
    pr_info("[INIT] Tasklet ready\n");

    /* ------------------------------------------------------------------
     * Step 6: Workqueue
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 6/9 — Creating workqueue \"mychardev_wq\"\n");
    my_workqueue = create_singlethread_workqueue("mychardev_wq");
    if (!my_workqueue) {
        pr_alert("[INIT] FAILED step 6 — create_singlethread_workqueue() "
                 "returned NULL\n");
        ClearPageReserved(virt_to_page((void *)dma_buffer_virt));
        free_pages(dma_buffer_virt, 0);
        free_pages(mmap_buffer, 0);
        kfree(device_buffer);
        return -ENOMEM;
    }
    INIT_WORK(&my_work, device_work_handler);
    pr_info("[INIT] Workqueue created\n");

    /* ------------------------------------------------------------------
     * Step 7: Allocate character device number
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 7/9 — Allocating char device numbers\n");
    ret = alloc_chrdev_region(&dev_number, 0, NUM_DEVICES, DEVICE_NAME);
    if (ret < 0) {
        pr_alert("[INIT] FAILED step 7 — alloc_chrdev_region() returned %d\n",
                 ret);
        destroy_workqueue(my_workqueue);
        ClearPageReserved(virt_to_page((void *)dma_buffer_virt));
        free_pages(dma_buffer_virt, 0);
        free_pages(mmap_buffer, 0);
        kfree(device_buffer);
        return ret;
    }

    /*
     * WHY LOG MAJOR/MINOR:
     * Cross-check with /proc/devices to confirm registration:
     *   cat /proc/devices | grep mychardev
     * The major number is the key link between the device file and
     * this driver in the kernel's device table.
     */
    pr_info("[INIT] Device numbers — major: %d  minor: %d\n",
            MAJOR(dev_number), MINOR(dev_number));

    /* ------------------------------------------------------------------
     * Step 8: Register cdev + create /dev node
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 8/9 — Registering cdev and creating /dev node\n");

    cdev_init(&my_cdev, &fops);
    my_cdev.owner = THIS_MODULE;

    ret = cdev_add(&my_cdev, dev_number, NUM_DEVICES);
    if (ret < 0) {
        pr_alert("[INIT] FAILED step 8 — cdev_add() returned %d\n", ret);
        unregister_chrdev_region(dev_number, NUM_DEVICES);
        destroy_workqueue(my_workqueue);
        ClearPageReserved(virt_to_page((void *)dma_buffer_virt));
        free_pages(dma_buffer_virt, 0);
        free_pages(mmap_buffer, 0);
        kfree(device_buffer);
        return ret;
    }

    my_class = class_create(DEVICE_NAME);
    if (IS_ERR(my_class)) {
        pr_alert("[INIT] FAILED step 8 — class_create() returned %ld\n",
                 PTR_ERR(my_class));
        cdev_del(&my_cdev);
        unregister_chrdev_region(dev_number, NUM_DEVICES);
        destroy_workqueue(my_workqueue);
        ClearPageReserved(virt_to_page((void *)dma_buffer_virt));
        free_pages(dma_buffer_virt, 0);
        free_pages(mmap_buffer, 0);
        kfree(device_buffer);
        return PTR_ERR(my_class);
    }

    if (IS_ERR(device_create(my_class, NULL, dev_number, NULL, DEVICE_NAME))) {
        pr_alert("[INIT] FAILED step 8 — device_create() could not make "
                 "/dev/%s\n", DEVICE_NAME);
        class_destroy(my_class);
        cdev_del(&my_cdev);
        unregister_chrdev_region(dev_number, NUM_DEVICES);
        destroy_workqueue(my_workqueue);
        ClearPageReserved(virt_to_page((void *)dma_buffer_virt));
        free_pages(dma_buffer_virt, 0);
        free_pages(mmap_buffer, 0);
        kfree(device_buffer);
        return -1;
    }

    pr_info("[INIT] /dev/%s created (major %d, minor %d)\n",
            DEVICE_NAME, MAJOR(dev_number), MINOR(dev_number));

    /* ------------------------------------------------------------------
     * Step 9: IRQ handler
     * ------------------------------------------------------------------ */
    pr_info("[INIT] Step 9/9 — Registering IRQ %d handler\n", SIMULATED_IRQ);
    ret = request_irq(SIMULATED_IRQ, device_irq_handler,
                      IRQF_SHARED, DEVICE_NAME, &my_cdev);
    if (ret) {
        /*
         * WHY pr_warn (NOT pr_alert) FOR IRQ FAILURE:
         * IRQ failure is serious but the remaining driver paths
         * (read/write/ioctl/mmap) still work.  In a production
         * driver you would almost certainly return an error here.
         */
        pr_warn("[INIT] request_irq(%d) failed (ret=%d) — "
                "interrupt-driven features disabled\n", SIMULATED_IRQ, ret);
    } else {
        pr_info("[INIT] IRQ %d registered successfully\n", SIMULATED_IRQ);
    }

    /* ------------------------------------------------------------------
     * Load-complete summary
     * ------------------------------------------------------------------ */
    pr_notice("========================================\n");
    pr_notice("Driver loaded successfully!\n");
    pr_notice("  Device    : /dev/%s\n",    DEVICE_NAME);
    pr_notice("  Major     : %d\n",          MAJOR(dev_number));
    pr_notice("  RW buf    : %p  (%d B)\n",  device_buffer, BUFFER_SIZE);
    pr_notice("  MMAP buf  : virt=0x%lx  phys=0x%llx  (%lu B)\n",
              mmap_buffer,
              (unsigned long long)virt_to_phys((void *)mmap_buffer),
              MMAP_SIZE);
    pr_notice("  DMA buf   : virt=0x%lx  phys=0x%llx  (%lu B)\n",
              dma_buffer_virt,
              (unsigned long long)dma_buffer_phys,
              (unsigned long)DMA_BUFFER_SIZE);
    pr_notice("  IRQ       : %d\n",          SIMULATED_IRQ);
    pr_notice("  pr_debug  : echo \"file char_driver.c +p\" > "
              "/sys/kernel/debug/dynamic_debug/control\n");
    pr_notice("========================================\n");

    return 0;
}

/* ============================================================================
 * MODULE EXIT
 * ============================================================================
 *
 * LOG LEVELS:
 *   pr_notice — lifecycle banners (always visible)
 *   pr_info   — per-step progress and final event counters
 *
 * WHY LOG EVERY CLEANUP STEP:
 *   If cleanup crashes (double-free, use-after-free, page flag corruption),
 *   the last log line before the crash tells you exactly where.
 *   LOG THE ADDRESS BEFORE FREEING — if free_pages() panics, the
 *   virtual and physical addresses are already in dmesg for analysis.
 */
static void __exit chardev_exit(void)
{
    pr_notice("========================================\n");
    pr_notice("Unloading %s driver\n", DEVICE_NAME);
    pr_notice("========================================\n");

    /* -- Tasklet --------------------------------------------------------- */
    pr_info("[EXIT] Killing tasklet (ran %lu times)...\n", tasklet_count);
    tasklet_kill(&my_tasklet);
    pr_info("[EXIT] Tasklet killed\n");

    /* -- Workqueue ------------------------------------------------------- */
    if (my_workqueue) {
        pr_info("[EXIT] Destroying workqueue (processed %lu items)...\n",
                work_count);
        destroy_workqueue(my_workqueue);
        pr_info("[EXIT] Workqueue destroyed\n");
    }

    /* -- IRQ ------------------------------------------------------------- */
    pr_info("[EXIT] Freeing IRQ %d (handled %lu times)...\n",
            SIMULATED_IRQ, irq_count);
    free_irq(SIMULATED_IRQ, &my_cdev);
    pr_info("[EXIT] IRQ freed\n");

    /* -- Device node + class --------------------------------------------- */
    pr_info("[EXIT] Removing /dev/%s and device class...\n", DEVICE_NAME);
    device_destroy(my_class, dev_number);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_number, NUM_DEVICES);
    pr_info("[EXIT] Device node and class removed\n");

    /* -- DMA buffer ------------------------------------------------------- */
    if (dma_buffer_virt) {
        /*
         * WHY LOG ADDRESS BEFORE FREE:
         * If ClearPageReserved() or free_pages() triggers a kernel
         * warning (e.g. bad page flags, double-free), this log entry
         * is already in dmesg, telling you which exact allocation was
         * involved.  Log FIRST, then free.
         */
        pr_info("[EXIT] Freeing DMA buffer — "
                "virt: 0x%lx  phys: 0x%llx\n",
                dma_buffer_virt, (unsigned long long)dma_buffer_phys);

        ClearPageReserved(virt_to_page((void *)dma_buffer_virt));
        free_pages(dma_buffer_virt, 0);

        dma_buffer_virt = 0; /* Poison to catch use-after-free */
        dma_buffer_phys = 0;
        pr_info("[EXIT] DMA buffer freed\n");
    }

    /* -- MMAP buffer ------------------------------------------------------ */
    if (mmap_buffer) {
        pr_info("[EXIT] Freeing mmap buffer — "
                "virt: 0x%lx  phys: 0x%llx\n",
                mmap_buffer,
                (unsigned long long)virt_to_phys((void *)mmap_buffer));
        free_pages(mmap_buffer, 0);
        mmap_buffer = 0;
        pr_info("[EXIT] MMAP buffer freed\n");
    }

    /* -- Read/write buffer ----------------------------------------------- */
    if (device_buffer) {
        pr_info("[EXIT] Freeing read/write buffer at %p\n", device_buffer);
        kfree(device_buffer);
        device_buffer = NULL;
        pr_info("[EXIT] Read/write buffer freed\n");
    }

    /* -- Unload summary -------------------------------------------------- */
    pr_notice("========================================\n");
    pr_notice("Driver unloaded. Final event counters:\n");
    pr_notice("  IRQs handled    : %lu\n", irq_count);
    pr_notice("  Tasklet runs    : %lu\n", tasklet_count);
    pr_notice("  Workqueue items : %lu\n", work_count);
    pr_notice("========================================\n");
}

module_init(chardev_init);
module_exit(chardev_exit);

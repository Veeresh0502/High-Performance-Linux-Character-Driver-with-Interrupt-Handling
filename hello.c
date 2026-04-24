/*
 * hello.c - Simple Linux Kernel Module (Hello World)
 * 
 * This module demonstrates the basics of kernel module development:
 * - Loading and unloading a module
 * - Printing messages to kernel log
 * - Module metadata (license, author, description)
 */

/* Essential header for all kernel modules */
#include <linux/module.h>

/* Header for KERN_INFO and printk() */
#include <linux/kernel.h>

/* Header for __init and __exit macros */
#include <linux/init.h>

/*
 * MODULE METADATA
 * This information appears when you run 'modinfo hello.ko'
 */

/* Required: Specifies the license type (GPL is most common) */
MODULE_LICENSE("GPL");

/* Author information */
MODULE_AUTHOR("Your Name");

/* Brief description of what this module does */
MODULE_DESCRIPTION("A simple Hello World kernel module");

/* Module version */
MODULE_VERSION("1.0");

/*
 * hello_init() - Module initialization function
 * 
 * This function is called when the module is loaded into the kernel
 * using 'insmod' or 'modprobe' commands.
 * 
 * __init macro tells the kernel this function is only used during
 * initialization, so memory can be freed after module loads.
 * 
 * Return: 0 on success, negative error code on failure
 */
static int __init hello_init(void)
{
    /*
     * printk() is the kernel's version of printf()
     * It writes to the kernel log buffer (view with 'dmesg')
     * 
     * KERN_INFO is the log level (INFO, WARNING, ERR, etc.)
     * Don't use \n at start - it's automatically added
     */
    printk(KERN_INFO "Hello World: Module loaded successfully!\n");
    printk(KERN_INFO "Hello World: Kernel module is now running.\n");
    
    /* Return 0 indicates successful initialization */
    return 0;
}

/*
 * hello_exit() - Module cleanup function
 * 
 * This function is called when the module is removed from the kernel
 * using 'rmmod' command.
 * 
 * __exit macro tells the kernel this function is only used during
 * module removal, allowing the kernel to optimize memory.
 */
static void __exit hello_exit(void)
{
    /*
     * Clean up and print goodbye message
     * In a real driver, you'd also free memory, unregister devices, etc.
     */
    printk(KERN_INFO "Hello World: Module unloaded. Goodbye!\n");
}

/*
 * REGISTER MODULE ENTRY AND EXIT POINTS
 * 
 * These macros tell the kernel which functions to call when
 * loading and unloading the module.
 */

/* Register the initialization function */
module_init(hello_init);

/* Register the cleanup function */
module_exit(hello_exit);

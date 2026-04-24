# ⚙️ High-Performance Linux Character Driver with Interrupt Handling

## 📌 Project Overview

This project implements a **Linux Kernel Module (LKM)** for a **character device driver** enabling secure and efficient communication between **user space and kernel space**.

The driver supports advanced features such as **interrupt handling, synchronization, blocking I/O, memory mapping, and IOCTL operations**, demonstrating real-world kernel programming concepts.

---

## 🚀 Features

* 📥 Character device driver (`/dev` interface)
* 🔄 Read / Write operations
* 🔐 Safe user-kernel data transfer using `copy_to_user()` / `copy_from_user()`
* ⚡ Interrupt handling:

  * Top-half (Interrupt Service Routine)
  * Bottom-half (Tasklets / Workqueues)
* 🧵 Concurrency control using:

  * Mutex
  * Spinlock
* ⏳ Blocking I/O using Wait Queues
* 🧠 IOCTL interface for custom commands
* 🗺️ Memory mapping (`mmap`) support
* 🐞 Kernel debugging using `printk` and `dmesg`

---

## 🛠️ Technologies Used

* C Programming
* Linux Kernel Module (LKM)
* Ubuntu / WSL
* VS Code
* Git
* Kernel Debugging Tools (`dmesg`, `printk`)

---

## 📂 Project Structure

```id="drv1"
.
├── char_driver.c          # Main driver implementation
├── hello.c                # Basic kernel module example
├── Makefile               # Build configuration
├── test_blocking_read.c   # Blocking I/O test
├── test_ioctl.c           # IOCTL test
├── test_mmap.c            # Memory mapping test
├── test_blocking_read     # Compiled binary
├── test_ioctl             # Compiled binary
├── test_mmap              # Compiled binary
```

---

## ⚙️ Build & Run Instructions

### 🔧 Compile Kernel Module

```bash id="drv2"
make
```

---

### 📦 Insert Module

```bash id="drv3"
sudo insmod char_driver.ko
```

---

### 📜 Check Kernel Logs

```bash id="drv4"
dmesg | tail
```

---

### 🧾 Verify Device

```bash id="drv5"
ls /dev | grep char
```

---

### ▶️ Run Test Programs

```bash id="drv6"
gcc test_blocking_read.c -o test_blocking_read
./test_blocking_read

gcc test_ioctl.c -o test_ioctl
./test_ioctl

gcc test_mmap.c -o test_mmap
./test_mmap
```

---

### ❌ Remove Module

```bash id="drv7"
sudo rmmod char_driver
```

---

## 🧠 Key Learnings

* Deep understanding of **Linux kernel internals**
* Difference between **process context vs interrupt context**
* Safe **user-kernel communication mechanisms**
* Implementation of **synchronization primitives**
* Handling **interrupt-driven execution**
* Kernel debugging using `dmesg` and `printk`

---

## ⚠️ Challenges Faced

* Avoiding race conditions in concurrent environments
* Managing interrupt context safely
* Implementing efficient blocking mechanisms
* Debugging kernel-level issues without crashing the system

---

## 🔮 Future Improvements

* Support for multiple devices
* Poll/select system call integration
* Performance benchmarking
* Integration with real hardware interrupts

---

## 👨‍💻 Author

**Veeresh T**
GitHub: https://github.com/Veeresh0502

---

## 📌 Note

This project is developed for **learning and demonstrating Linux device driver development concepts**.

---

## ⭐ License

Open-source for educational purposes.

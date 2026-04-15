# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Rishaan D | PES2UG24AM134 |
| Namit Renjith  | PES2UG24AM097 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. WSL is not supported.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git
```

### Clone and build

```bash
git clone https://github.com/rishaandeen-sys/OS-Jackfruit.git
cd OS-Jackfruit/boilerplate
make
```

This produces: `engine`, `memory_hog`, `cpu_hog`, `io_pulse`, and `monitor.ko`

### Prepare root filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Copy workload binaries into base rootfs before creating per-container copies
cp memory_hog cpu_hog io_pulse rootfs-base/

# Create per-container writable copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

### Load the kernel module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
dmesg | tail
```

### Start the supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor binds a UNIX socket at `/tmp/mini_runtime.sock` and enters its accept loop.

### Use the CLI (Terminal 2)

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

sudo ./engine ps

sudo ./engine run logtest ./rootfs-alpha "/cpu_hog 5"

sudo ./engine logs alpha

sudo ./engine stop alpha
sudo ./engine stop beta
```

### Run memory limit tests

```bash
sudo ./engine start memtest ./rootfs-alpha "/memory_hog" --soft-mib 40 --hard-mib 60
sudo ./engine ps
dmesg | tail
```

### Run scheduling experiments

```bash
sudo ./engine start c1 ./rootfs-alpha "/cpu_hog 10" --nice 0
sudo ./engine start c2 ./rootfs-beta  "/cpu_hog 10" --nice 10

sudo ./engine start cpu ./rootfs-alpha "/cpu_hog 10"
sudo ./engine start io  ./rootfs-beta  "/io_pulse 20 200"
```

### Cleanup

```bash
sudo rmmod monitor
dmesg | tail
```

---

## 3. Demo with Screenshots

### Screenshot 1 — Multi-container supervision

![Supervisor Started](OS-ss/task1_1.png)

The supervisor process starts and initializes the container runtime.

![Two Containers Running](OS-ss/task1_2.png)

Two containers (`alpha` and `beta`) are running under the supervisor.

![Namespace Isolation](OS-ss/task2.png)

Each container operates in its own PID and UTS namespace.

![Filesystem Isolation](OS-ss/task3.png)

Each container has its own isolated filesystem.

---

### Screenshot 2 — Metadata tracking

![Metadata 1](OS-ss/task4_1.png)

Container metadata such as PID and state is tracked.

![Metadata 2](OS-ss/task4_2.png)

Metadata updates dynamically as containers run.

---

### Screenshot 3 — Resource control

![Resource Control](OS-ss/task5.png)

Containers run with defined CPU and memory limits.

---

### Screenshot 4 — Memory monitoring

![Memory 1](OS-ss/task6_1.png)

![Memory 2](OS-ss/task6_2.png)

![Memory 3](OS-ss/task6_3.png)

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime uses `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` to create isolated containers.

PID namespace gives each container its own PID space starting at 1.  
UTS namespace provides separate hostnames.  
Mount namespace with `chroot()` ensures filesystem isolation.

---

### 4.2 Supervisor and Lifecycle

A long-running supervisor prevents zombie processes, tracks metadata, and manages logging.

---

### 4.3 IPC and Synchronisation

Uses UNIX sockets for control and pipes for logging.  
A bounded buffer with mutex and condition variables ensures safe multithreading.

---

### 4.4 Memory Management

RSS is tracked in kernel space.  
Soft limit logs warnings.  
Hard limit kills process using `SIGKILL`.

---

### 4.5 Scheduling Behaviour

CFS allocates CPU based on weights.  
Lower nice value → more CPU time.  
I/O processes get priority when waking.

---

## 5. Design Decisions

- Used namespaces for isolation  
- Supervisor for lifecycle management  
- Kernel module for memory enforcement  
- Pipes + sockets for IPC  

---

## 6. Conclusion

The project demonstrates container isolation, resource management, and scheduling behaviour using Linux primitives.

---
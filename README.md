# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

---

## 1. Team Information

Name 1: Rishaan D  
SRN 1: PES2UG24AM134  

Name 2: Namit Renjith  
SRN 2: PES2UG24AM097  

---

## 2. Build, Load, and Run Instructions

### Build

```bash
cd boilerplate
make
```

---

### Load Kernel Module

```bash
sudo insmod monitor.ko
lsmod | grep monitor
```

---

### Start Supervisor (Terminal 1)

```bash
./engine
```

---

### Prepare per-container rootfs copies (Terminal 2)

```bash
cp -r rootfs rootfs-alpha
cp -r rootfs rootfs-beta
```

---

### Copy workload binaries into rootfs before launch

```bash
cp cpu_hog rootfs-alpha/
cp io_pulse rootfs-beta/
cp memory_hog rootfs-alpha/
```

---

### Launch containers (Terminal 2)

```bash
./engine start alpha ./cpu_hog 4096 40
./engine start beta ./io_pulse 4096 40
```

---

### List containers

```bash
./engine ps
```

---

### View logs

```bash
dmesg | tail
```

---

### Stop containers

```bash
./engine stop alpha
./engine stop beta
```

---

## 3. Screenshots

All required screenshots are included in the `OS-ss` folder.

---

## 4. Notes

- Kernel module (`monitor.c`) tracks memory usage.
- `engine.c` acts as the container supervisor.
- Multiple workloads (CPU, IO, memory) are supported.

---
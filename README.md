# Framebuffer-Driver

A Linux character device driver implementing a virtualized framebuffer with per-handle viewports and atomic write operations.

## Overview
This driver creates a virtual memory area in the kernel that acts as a framebuffer. Unlike standard framebuffers, it allows each process (via its file descriptor) to define a specific "viewport" or window into the global buffer.

## Key Features
- **Dynamic Resizing**: Use `FB536_IOCTSETSIZE` to change the global dimensions (width/height) on the fly.
- **Coordinate Translation**: The driver handles the math to map local viewport coordinates to global framebuffer offsets during reads and writes.
- **Mathematical Write Modes**: Supports `SET`, `ADD`, `SUB`, `AND`, `OR`, and `XOR` operations. `ADD` and `SUB` are saturation-aware (clamped at 0 and 255).
- **Intersection-Based Notifications**: Processes can block using `FB536_IOCWAIT`. They are only woken up if a write operation overlaps with their specific viewport.

## Module Parameters
- `numminors`: Number of devices to create (default 4).
- `width`/`height`: Initial dimensions (default 1000x1000).

## Installation
1. Compile using `make`.
2. Load with `insmod fb536.ko`.
3. Create nodes using `mknod` based on the major number in `/proc/devices`.

## Technical Implementation
The driver uses a `mutex` to protect the global framebuffer data and a `spinlock` for the internal list of open file handles (`file_list`). Intersection logic is handled in `viewports_intersect` to minimize unnecessary wakeups.

**Author:** Ozan Yanik
**License:** GPL

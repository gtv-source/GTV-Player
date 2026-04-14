# ===========

# GTV-Player

# ===========

GTV-Player is a customized version of QEMU, a generic and open-source machine emulator and virtualizer.

This project is based on the original QEMU source code and has been modified to provide a tailored user experience and additional features.

---

## Overview

Like QEMU, GTV-Player is capable of emulating a complete machine in software without requiring hardware virtualization support. By using dynamic translation, it can achieve good performance across multiple architectures.

When used with supported hypervisors (such as KVM, Xen, or WHPX), it can provide near-native CPU performance while still emulating hardware devices.

GTV-Player can also run binaries compiled for different architectures by emulating CPU instructions and system calls.

---

## Modifications

This project includes several custom modifications on top of QEMU:

* Custom SDL-based user interface
* Additional source files (e.g. `abc.c`)
* Modified input and device handling
* Custom behavior and optimizations for specific use cases

These changes are intended to improve usability and integration for end users.

---

## License

GTV-Player is based on QEMU and is therefore licensed under the GNU General Public License version 2 (GPLv2).

* Original QEMU source code:
  Copyright (c) Fabrice Bellard and the QEMU Project

* Modifications in GTV-Player:
  Copyright (c) 2026 GTV-Player

You are free to use, modify, and redistribute this software under the terms of the GPLv2.

The full license text can be found in the LICENSE file.

---

## Source Code Availability

In compliance with the GPLv2 license, the complete corresponding source code for this project, including all modifications, is available at:

👉 https://github.com/gtv-source/GTV-Player

---

## Building

GTV-Player can be built similarly to QEMU:

```bash
mkdir build
cd build
../configure
make
```

Refer to the original QEMU documentation for more advanced build configurations.

---

## Documentation

Original QEMU documentation is available at:
https://www.qemu.org/documentation/

Some features in GTV-Player may differ due to custom modifications.

---

## Credits

This project is based on QEMU, originally developed by Fabrice Bellard and the QEMU community.

---

## Disclaimer

This project is an independent modification of QEMU and is not affiliated with or endorsed by the original QEMU project.

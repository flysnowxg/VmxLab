# VmxLab

A minimal Windows x64 kernel driver for learning Intel VT-x / VMX virtualization.

## Highlights

- **Minimal working hypervisor**: Demonstrates the complete VMX lifecycle — VMXON → VMCLEAR → VMPTRLD → VMLAUNCH → VMRESUME → VM Exit → VMXOFF — with no unnecessary code
- **Guest executes real computation**: The guest runs `RAX = RCX + RDX` (integer addition) and returns the result to the host via VMCALL, validating Host/Guest data passing end-to-end
- **VMware nested virtualization compatible**: `AdjustCtrl()` reads MSR allowed-0/allowed-1 bits to automatically adjust control fields, allowing the driver to run inside a VMware VM (requires "Virtualize Intel VT-x/EPT")
- **Pure WDM driver**: No KMDF dependency — clean and readable code, ideal for learning Windows kernel driver development from scratch
- **User-mode CLI client**: Communicates with the driver via IOCTL control codes, making it easy to extend with new experiments

## Tech Stack

- **Platform**: Windows 10 x64 kernel mode
- **Toolchain**: VS2015 + WDK 10 (10.0.15063.0)
- **ISA extensions**: Intel VT-x (VMX), VMCS, MSR read/write, x64 MASM inline assembly
- **Concepts covered**: VMXON/VMXOFF, VMLAUNCH/VMRESUME, VMCALL, VM Exit, VMCS Host/Guest/Control fields, EPT (reserved), IA32_EFER, IA32_PAT, segment descriptors, GDT/IDT

## Repository Layout

```
VmxLab/
├── driver/             Kernel driver source (.sys)
│   ├── driver.c        Core driver logic: VMX lifecycle management
│   ├── driver.h        VMCS field numbers, MSR constants, function declarations
│   ├── driver_asm.asm  VMX assembly wrappers: VMLAUNCH, VM Exit Handler, Guest entry
│   ├── hvIoctl.h       IOCTL definitions shared between driver and client
│   └── driver.vcxproj
├── client/             User-mode CLI client (.exe)
│   ├── client.c        DeviceIoControl to trigger experiments and read results
│   └── client.vcxproj
├── VmxLab.sln   VS2015 solution
├── build.bat           One-click driver build
└── VmxLab.bat   Driver deploy / load / unload helper
```

## Quick Start

### Requirements

- Windows 10 x64 (physical machine or VMware VM)
- Visual Studio 2015 + WDK 10

### Deploy and Run

```Config Env
:: Enable test signing (one-time reboot required)
bcdedit /set testsigning on
Install the test certificate VmxLab.cer into the local root certificate store.

:: Register and start the driver
VmxLab.bat install
VmxLab.bat start

:: Run Vm,Caculate 3+3+1+1
VmxLabClient.exe 3 3

:: Uninstall
VmxLab.bat uninstall

```

Monitor `[VMX]` output with **DebugView** (Sysinternals) — enable Kernel Capture.

### VMware Setup

VM Settings → Processors → check **Virtualize Intel VT-x/EPT**.

## Keywords

`Intel VT-x` `VMX` `Hypervisor` `Windows Kernel Driver` `WDK` `VMCS` `VMLAUNCH` `VMCALL` `VM Exit` `Nested Virtualization` `VMware` `x64` `Ring-0` `learning project`

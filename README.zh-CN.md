# VmxLab

- 一个极简的 Windows x64 内核驱动，用于学习 Intel VT-x / VMX 虚拟化技术。
- 本项目实现的功能是：
- 通过windows驱动从系统内核通过vt-x 技术进入客户机状态。然后在客户机中计算3次加法
- 第1次进入客户机时计算 用户通过 VmxLabClient.exe 命令行传入的的两个整数之和
- 第2次进入客户机时 将上一步的计算结果再加1
- 第3次进入客户机时 将上一步的计算结果再次加1
- 然后销毁客户机配置,退出虚拟化环境
- 然后将计算结果返回给VmxLabClient.exe 


## 项目特点

- **最小可运行的 Hypervisor**：完整演示 VMX 生命周期 —— VMXON → VMCLEAR → VMPTRLD → VMLAUNCH → VMRESUME → VM Exit → VMXOFF，无任何多余代码
- **Guest 执行真实计算**：客户机执行 `RAX = RCX + RDX`（整数加法），通过 VMCALL 将结果返回宿主机，验证 Host/Guest 数据传递
- **兼容 VMware 嵌套虚拟化**：通过 `AdjustCtrl()` 读取 MSR 允许位自动调整控制字段，可在 VMware 虚拟机中运行（需开启 "Virtualize Intel VT-x/EPT"）
- **纯 WDM 驱动**：无 KMDF 依赖，代码结构清晰，适合从零学习 Windows 内核驱动开发
- **用户态 CLI 客户端**：通过 IOCTL 控制码与驱动交互，方便扩展更多实验

## 技术栈

- **平台**：Windows 10 x64 内核模式
- **工具链**：VS2015 + WDK 10 (10.0.15063.0)
- **指令集扩展**：Intel VT-x (VMX)、VMCS、MSR 读写、x64 MASM 内联汇编
- **涉及概念**：VMXON/VMXOFF、VMLAUNCH/VMRESUME、VMCALL、VM Exit、VMCS Host/Guest/Control 字段、EPT（预留）、IA32_EFER、IA32_PAT、段描述符、GDT/IDT

## 目录结构

```
VmxLab/
├── driver/             内核驱动源码（.sys）
│   ├── driver.c        主驱动逻辑：VMX 生命周期管理
│   ├── driver.h        VMCS 字段号、MSR 常量、函数声明
│   ├── driver_asm.asm  VMX 汇编封装：VMLAUNCH、VM Exit Handler、Guest 入口
│   ├── hvIoctl.h       驱动与客户端共享的 IOCTL 定义
│   └── driver.vcxproj
├── client/             用户态 CLI 客户端（.exe）
│   ├── client.c        DeviceIoControl 触发实验、读取结果
│   └── client.vcxproj
├── VmxLab.sln   VS2015 解决方案
├── build.bat           一键构建驱动
└── VmxLab.bat   驱动部署 / 加载 / 卸载工具
```

## 快速开始

### 环境要求

- Windows 10 x64（物理机或 VMware 虚拟机）
- Visual Studio 2015 + WDK 10

输出：`driver\x64\Debug\driver.sys`

### 部署与运行

```配置环境
:: 开启测试签名（首次需重启）
bcdedit /set testsigning on
安装测试证书 VmxLab.cer 到本地根证书存储区

:: 注册并启动驱动
VmxLab.bat install
VmxLab.bat start

:: 运行虚拟机并计算 a+b+1+1
VmxLabClient.exe 3 3

:: 卸载
VmxLab.bat uninstall
```

用 **DebugView**（Sysinternals，需开启 Kernel Capture）观察 `[VMX]` 输出。

### VMware 配置

虚拟机设置 → 处理器 → 勾选 **Virtualize Intel VT-x/EPT**。

## 关键词

`Intel VT-x` `VMX` `Hypervisor` `Windows Kernel Driver` `WDK` `VMCS` `VMLAUNCH` `VMCALL` `VM Exit` `Nested Virtualization` `VMware` `x64` `Ring-0` `学习项目`

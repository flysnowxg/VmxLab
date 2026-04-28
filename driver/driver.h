// VmxLab author:flysnowxg email:308821698@qq.com date:20251205
#pragma once
#include <ntddk.h>
#include <intrin.h>

// -------------------------------------------------------
// MSR 编号
// -------------------------------------------------------
#define MSR_IA32_VMX_BASIC          0x480
#define MSR_IA32_FEATURE_CONTROL    0x3A
#define MSR_IA32_VMX_CR0_FIXED0     0x486
#define MSR_IA32_VMX_CR0_FIXED1     0x487
#define MSR_IA32_VMX_CR4_FIXED0     0x488
#define MSR_IA32_VMX_CR4_FIXED1     0x489
#define MSR_IA32_FS_BASE            0xC0000100
#define MSR_IA32_GS_BASE            0xC0000101
#define MSR_IA32_SYSENTER_CS        0x174
#define MSR_IA32_SYSENTER_ESP       0x175
#define MSR_IA32_SYSENTER_EIP       0x176
#define MSR_IA32_EFER               0xC0000080  /* EFER: LME=bit8, LMA=bit10, SCE=bit0, NXE=bit11 */
#define MSR_IA32_PAT                0x277       /* Page Attribute Table */

// IA32_FEATURE_CONTROL 位
#define FEATURE_CONTROL_LOCK        (1ULL << 0)
#define FEATURE_CONTROL_VMXON       (1ULL << 2)

// -------------------------------------------------------
// VMCS 字段编号
// -------------------------------------------------------

// 16-bit Guest
#define VMCS_GUEST_ES_SEL                       0x0800
#define VMCS_GUEST_CS_SEL                       0x0802
#define VMCS_GUEST_SS_SEL                       0x0804
#define VMCS_GUEST_DS_SEL                       0x0806
#define VMCS_GUEST_FS_SEL                       0x0808
#define VMCS_GUEST_GS_SEL                       0x080A
#define VMCS_GUEST_LDTR_SEL                     0x080C
#define VMCS_GUEST_TR_SEL                       0x080E

// 16-bit Host
#define VMCS_HOST_ES_SEL                        0x0C00
#define VMCS_HOST_CS_SEL                        0x0C02
#define VMCS_HOST_SS_SEL                        0x0C04
#define VMCS_HOST_DS_SEL                        0x0C06
#define VMCS_HOST_FS_SEL                        0x0C08
#define VMCS_HOST_GS_SEL                        0x0C0A
#define VMCS_HOST_TR_SEL                        0x0C0C

// 64-bit Control
#define VMCS_CTRL_MSR_BITMAP                    0x2004  /* MSR-bitmap physical address (when "use MSR bitmaps" bit28 of proc ctrl = 1) */

// 64-bit Guest
#define VMCS_GUEST_VMCS_LINK_PTR                0x2800
#define VMCS_GUEST_IA32_DEBUGCTL                0x2802
#define VMCS_GUEST_IA32_PAT                     0x2804
#define VMCS_GUEST_IA32_EFER                    0x2806  /* must have LMA=1 when IA-32e mode + CR0.PG=1 */

// 64-bit Host MSR (needed when exit controls force "load PAT/EFER" on exit)
#define VMCS_HOST_IA32_PAT                      0x2C00
#define VMCS_HOST_IA32_EFER                     0x2C02

// 32-bit Control
#define VMCS_CTRL_PIN_EXEC                      0x4000
#define VMCS_CTRL_PROC_EXEC                     0x4002
#define VMCS_CTRL_EXCEPTION_BITMAP              0x4004
#define VMCS_CTRL_VMEXIT_CTRL                   0x400C
#define VMCS_CTRL_VMEXIT_MSR_STORE_COUNT        0x400E
#define VMCS_CTRL_VMEXIT_MSR_LOAD_COUNT         0x4010
#define VMCS_CTRL_VMENTRY_CTRL                  0x4012
#define VMCS_CTRL_VMENTRY_MSR_LOAD_COUNT        0x4014
#define VMCS_CTRL_VMENTRY_INTR_INFO             0x4016
#define VMCS_CTRL_PROC_EXEC2                    0x401E

// 32-bit Read-Only
#define VMCS_RO_VM_INSTRUCTION_ERROR            0x4400
#define VMCS_RO_EXIT_REASON                     0x4402
#define VMCS_RO_VM_EXIT_INTR_INFO               0x4404
#define VMCS_RO_VM_EXIT_INTR_ERROR_CODE         0x4406
#define VMCS_RO_EXIT_QUALIFICATION              0x6400
#define VMCS_RO_GUEST_LINEAR_ADDR               0x640A

// 32-bit Guest
#define VMCS_GUEST_ES_LIMIT                     0x4800
#define VMCS_GUEST_CS_LIMIT                     0x4802
#define VMCS_GUEST_SS_LIMIT                     0x4804
#define VMCS_GUEST_DS_LIMIT                     0x4806
#define VMCS_GUEST_FS_LIMIT                     0x4808
#define VMCS_GUEST_GS_LIMIT                     0x480A
#define VMCS_GUEST_LDTR_LIMIT                   0x480C
#define VMCS_GUEST_TR_LIMIT                     0x480E
#define VMCS_GUEST_GDTR_LIMIT                   0x4810
#define VMCS_GUEST_IDTR_LIMIT                   0x4812
#define VMCS_GUEST_ES_AR                        0x4814
#define VMCS_GUEST_CS_AR                        0x4816
#define VMCS_GUEST_SS_AR                        0x4818
#define VMCS_GUEST_DS_AR                        0x481A
#define VMCS_GUEST_FS_AR                        0x481C
#define VMCS_GUEST_GS_AR                        0x481E
#define VMCS_GUEST_LDTR_AR                      0x4820
#define VMCS_GUEST_TR_AR                        0x4822
#define VMCS_GUEST_INTERRUPTIBILITY             0x4824
#define VMCS_GUEST_ACTIVITY_STATE               0x4826
#define VMCS_GUEST_PENDING_DBG_EXCEPTIONS       0x6822
#define VMCS_GUEST_IA32_SYSENTER_CS             0x482A
#define VMCS_CTRL_CR3_TARGET_COUNT              0x400A

// 32-bit Host
#define VMCS_HOST_IA32_SYSENTER_CS              0x4C00

// 64-bit Natural Guest
#define VMCS_GUEST_CR0                          0x6800
#define VMCS_GUEST_CR3                          0x6802
#define VMCS_GUEST_CR4                          0x6804
#define VMCS_GUEST_ES_BASE                      0x6806
#define VMCS_GUEST_CS_BASE                      0x6808
#define VMCS_GUEST_SS_BASE                      0x680A
#define VMCS_GUEST_DS_BASE                      0x680C
#define VMCS_GUEST_FS_BASE                      0x680E
#define VMCS_GUEST_GS_BASE                      0x6810
#define VMCS_GUEST_LDTR_BASE                    0x6812
#define VMCS_GUEST_TR_BASE                      0x6814
#define VMCS_GUEST_GDTR_BASE                    0x6816
#define VMCS_GUEST_IDTR_BASE                    0x6818
#define VMCS_GUEST_DR7                          0x681A
#define VMCS_GUEST_RSP                          0x681C
#define VMCS_GUEST_RIP                          0x681E
#define VMCS_GUEST_RFLAGS                       0x6820
#define VMCS_GUEST_IA32_SYSENTER_ESP            0x6824
#define VMCS_GUEST_IA32_SYSENTER_EIP            0x6826
#define VMCS_GUEST_INSTRUCTION_LEN				0x400C

// 64-bit Natural Control (CR masks and shadows)
#define VMCS_CTRL_CR0_GUEST_HOST_MASK           0x6000
#define VMCS_CTRL_CR4_GUEST_HOST_MASK           0x6002
#define VMCS_CTRL_CR0_READ_SHADOW               0x6004
#define VMCS_CTRL_CR4_READ_SHADOW               0x6006

// 64-bit Natural Host
#define VMCS_HOST_CR0                           0x6C00
#define VMCS_HOST_CR3                           0x6C02
#define VMCS_HOST_CR4                           0x6C04
#define VMCS_HOST_FS_BASE                       0x6C06
#define VMCS_HOST_GS_BASE                       0x6C08
#define VMCS_HOST_TR_BASE                       0x6C0A
#define VMCS_HOST_GDTR_BASE                     0x6C0C
#define VMCS_HOST_IDTR_BASE                     0x6C0E
#define VMCS_HOST_IA32_SYSENTER_ESP             0x6C10
#define VMCS_HOST_IA32_SYSENTER_EIP             0x6C12
#define VMCS_HOST_RSP                           0x6C14
#define VMCS_HOST_RIP                           0x6C16

// VM Exit Reason
#define EXIT_REASON_VMCALL                      18

// VMEntry/VMExit 控制位（按需设置）
#define PIN_CTRL_MSR                            0x481
#define PROC_CTRL_MSR                           0x482
#define PROC_CTRL2_MSR                          0x48B
#define EXIT_CTRL_MSR                           0x483
#define ENTRY_CTRL_MSR                          0x484

/* TRUE MSR variants (use when VMX_BASIC bit 55 = 1) */
#define TRUE_PIN_CTRL_MSR                       0x48D
#define TRUE_PROC_CTRL_MSR                      0x48E
#define TRUE_EXIT_CTRL_MSR                      0x48F
#define TRUE_ENTRY_CTRL_MSR                     0x490

/* VMX_BASIC bit 55: if set, TRUE MSRs exist and must be used */
#define VMX_BASIC_TRUE_CTLS_BIT                 (1ULL << 55)

#define VMENTRY_CTRL_IA32E_GUEST                (1 << 9)
#define VMENTRY_CTRL_LOAD_IA32_PAT              (1 << 14) /* load guest PAT on entry */
#define VMENTRY_CTRL_LOAD_IA32_EFER             (1 << 15) /* load guest EFER on entry */
#define VMEXIT_CTRL_HOST_ADDR_SPACE             (1 << 9)
#define VMEXIT_CTRL_LOAD_IA32_PAT               (1 << 19) /* load host PAT on exit */
#define VMEXIT_CTRL_LOAD_IA32_EFER              (1 << 21) /* load host EFER on exit */

#define AR_FROM_LAR(lar)    ( ((lar) >> 8) & 0xF0FF )
#define AR_UNUSABLE         0x10000

// -------------------------------------------------------
// 汇编函数声明（vmx_asm.asm 实现）
// -------------------------------------------------------

// VMX 指令封装：返回 0=成功, 1=CF, 2=ZF
UINT8 AsmVmxOn(UINT64* pPhysAddr);
VOID AsmVmxOff(VOID);
UINT8 AsmVmClear(UINT64* pPhysAddr);
UINT8 AsmVmPtrld(UINT64* pPhysAddr);
UINT8 AsmVmWrite(ULONG_PTR field, ULONG_PTR value);
ULONG_PTR AsmVmRead(ULONG_PTR field);

// VMLAUNCH：在 asm 内设置 RCX/RDX 后执行
// 若 VMLAUNCH 成功 → VM Exit 后从 AsmVmExitHandler 经 g_HostRsp 返回到此处之后
// 若 VMLAUNCH 失败 → 直接返回（CF/ZF）
VOID AsmVmRun(UINT64 rcxVal, UINT64 rdxVal);

// 段寄存器读取（x64 必须用 asm）
UINT16 AsmGetCs(VOID);
UINT16 AsmGetSs(VOID);
UINT16 AsmGetDs(VOID);
UINT16 AsmGetEs(VOID);
UINT16 AsmGetFs(VOID);
UINT16 AsmGetGs(VOID);
UINT16 AsmGetTr(VOID);
UINT16 AsmGetLdtr(VOID);

// 描述符表
UINT64 AsmGetGdtrBase(VOID);
UINT16 AsmGetGdtrLimit(VOID);
UINT64 AsmGetIdtrBase(VOID);
UINT16 AsmGetIdtrLimit(VOID);
UINT32 AsmGetLar(UINT16 selector);//获取段属性(LAR)

// Guest 代码入口（取地址用，不被 C 直接调用）
VOID AsmGuestCodeEntry(VOID);
// Guest 退出后,宿主机的入口代码
VOID AsmVmExitHandler(VOID);

// -------------------------------------------------------
// C 全局变量&函数声明（vmx.c 实现）
// -------------------------------------------------------
// AsmVmRun 保存，AsmVmExitHandler 恢复
extern UINT64 g_HostRsp;
extern BOOLEAN g_VmLaunched;
// 被 AsmVmExitHandler 调用，guestRax = Guest 退出时的 RAX
VOID VmExitHandler(UINT64 guestRax);

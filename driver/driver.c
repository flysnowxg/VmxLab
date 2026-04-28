// VmxLab author:flysnowxg email:308821698@qq.com date:20251205
// driver.c - VmxLab Intel VT-x 学习驱动
//
// DriverEntry 只负责驱动基础设施（设备对象、IRP、卸载）。
// VMX 实验逻辑完全封装在 RunVmxTest() 中，由 IOCTL 触发。

#include "driver.h"
#include "hvIoctl.h"

// -------------------------------------------------------
// 设备名称
// -------------------------------------------------------
#define HVLAB_DEVICE_NAME   L"\\Device\\VmxLab"
#define HVLAB_SYMLINK_NAME  L"\\DosDevices\\VmxLab"

// -------------------------------------------------------
// 全局变量（仅 asm 与 VmExitHandler 之间的通信通道）
// -------------------------------------------------------
UINT64  g_GuestResult = 0;
UINT64 g_HostRsp = 0;  // AsmVmRun 保存，AsmVmExitHandler 恢复
BOOLEAN g_VmRunOccurred = FALSE;
BOOLEAN g_VmLaunched = FALSE;

// 驱动全局：设备对象与串行化互斥体
static PDEVICE_OBJECT g_DeviceObject = NULL;
static BOOLEAN        g_SymLinkCreated = FALSE;
static KMUTEX         g_VmxMutex;  // 串行化 IOCTL，保持 PASSIVE_LEVEL

// -------------------------------------------------------
// 从 GDT 描述符中获取段基地址
// -------------------------------------------------------
static UINT64 GetSegmentBase(UINT64 gdtBase, UINT16 selector) {
	PUINT8 e;
	UINT64 base;
	if ((selector & ~7) == 0) return 0;
	e = (PUINT8)(gdtBase + (UINT64)(selector & 0xFFF8));
	base = (UINT64)e[2]
		| ((UINT64)e[3] << 8)
		| ((UINT64)e[4] << 16)
		| ((UINT64)e[7] << 24);

	// 64 位模式下系统描述符为 16 字节，高 32 位在偏移 +8 处
	if ((e[5] & 0x10) == 0) {
		base |= (UINT64)(*(PUINT32)(e + 8)) << 32;
	}
	return base;
}

// -------------------------------------------------------
// 从 GDT 描述符中获取段界限（已展开为字节数）
// -------------------------------------------------------
static UINT32 GetSegmentLimit2(UINT64 gdtBase, UINT16 selector) {
	PUINT8 e;
	UINT32 limit;

	if ((selector & ~7) == 0)
		return 0;

	e = (PUINT8)(gdtBase + (UINT64)(selector & 0xFFF8));
	limit = (UINT32)e[0] | ((UINT32)e[1] << 8) | (((UINT32)e[6] & 0x0F) << 16);

	// G 位置 1 时，界限单位为 4KB 页
	if (e[6] & 0x80)
		limit = (limit << 12) | 0xFFF;

	return limit;
}

// -------------------------------------------------------
// 通过 LAR 指令获取段访问权限（Access Rights）
// -------------------------------------------------------
static UINT32 GetSegmentAr(UINT16 selector) {
	if ((selector & ~7) == 0) return AR_UNUSABLE;
	return AR_FROM_LAR(AsmGetLar(selector));
}

// -------------------------------------------------------
// 调整 CR0/CR4 以满足 VMX fixed-bit 约束
// -------------------------------------------------------
static ULONG_PTR AdjustCr0(ULONG_PTR v) {
	v |= (ULONG_PTR)__readmsr(MSR_IA32_VMX_CR0_FIXED0);
	v &= (ULONG_PTR)__readmsr(MSR_IA32_VMX_CR0_FIXED1);
	return v;
}

static ULONG_PTR AdjustCr4(ULONG_PTR v) {
	v |= (ULONG_PTR)__readmsr(MSR_IA32_VMX_CR4_FIXED0);
	v &= (ULONG_PTR)__readmsr(MSR_IA32_VMX_CR4_FIXED1);
	return v;
}

// -------------------------------------------------------
// 按照 MSR allowed-0/allowed-1 位调整 VMX 控制字段
// Low 32 bits  = 必须为 1 的位
// High 32 bits = 允许为 1 的位
// -------------------------------------------------------
static UINT32 AdjustCtrl(UINT32 desired, UINT32 msrIdx) {
	UINT64 msr = __readmsr(msrIdx);
	UINT32 must = (UINT32)(msr);
	UINT32 may = (UINT32)(msr >> 32);
	return (desired | must) & may;
}

// -------------------------------------------------------
// 步骤 1：检查当前 CPU 是否支持 VMX
//   - CPUID.1:ECX[5]  = VMX 功能位
//   - IA32_FEATURE_CONTROL[0] = Lock 位（必须为 1，否则 VMXON 触发 #GP）
//   - IA32_FEATURE_CONTROL[2] = 允许在 SMX 之外执行 VMXON
// -------------------------------------------------------
static NTSTATUS CheckVmxSupport(VOID) {
	int    cpuInfo[4];
	UINT64 featCtrl;

	// CPUID leaf 1, ECX bit5：VMX 支持标志
	__cpuid(cpuInfo, 1);
	if (!((cpuInfo[2] >> 5) & 1)) {
		DbgPrint("[VMX] CPUID: VMX not supported (ECX bit5=0)\n");
		return STATUS_NOT_SUPPORTED;
	}
	DbgPrint("[VMX] CPUID: VMX supported (ECX=0x%X)\n", cpuInfo[2]);

	// IA32_FEATURE_CONTROL：
	// bit0 (LOCK)  = 1：MSR 已由 BIOS 锁定，VMXON 才合法；
	// bit2 (VMXON) = 1：允许在 non-SMX 环境执行 VMXON。
	// 两位任一为 0，VMXON 均触发 #GP（BSOD）。
	featCtrl = __readmsr(MSR_IA32_FEATURE_CONTROL);
	DbgPrint("[VMX] IA32_FEATURE_CONTROL=0x%llX (LOCK=%llu VMXON=%llu)\n",
		featCtrl, featCtrl & 1, (featCtrl >> 2) & 1);

	if (!(featCtrl & FEATURE_CONTROL_LOCK)) {
		DbgPrint("[VMX] IA32_FEATURE_CONTROL not locked (bit0=0), VMXON would #GP\n");
		return STATUS_NOT_SUPPORTED;
	}
	if (!(featCtrl & FEATURE_CONTROL_VMXON)) {
		DbgPrint("[VMX] VMXON outside SMX disabled (bit2=0), VMXON would #GP\n");
		return STATUS_NOT_SUPPORTED;
	}

	DbgPrint("[VMX] VMX support confirmed\n");
	return STATUS_SUCCESS;
}

// -------------------------------------------------------
// 步骤 2：为 VMXON/VMCS 分配 4KB 物理连续内存
// -------------------------------------------------------
static NTSTATUS AllocVmxRegion(PVOID *ppVirt, UINT64 *pPhys, UINT32 revId) {
	PHYSICAL_ADDRESS low, high, skip;
	PVOID p;
	low.QuadPart = 0;
	high.QuadPart = (LONGLONG)0xFFFFFFFFFFFFFFFF;
	skip.QuadPart = PAGE_SIZE;

	p = MmAllocateContiguousMemorySpecifyCache(
		PAGE_SIZE,    // 分配 4KB
		low,          // 物理地址 >= 0
		high,         // 物理地址 <= 0xFFFF...（无限制）
		skip,         // 地址必须是 4KB 的整数倍
		MmNonCached   // 不经过 CPU 缓存，直接读写物理内存
		);
	if (!p) {
		DbgPrint("[VMX] MmAllocateContiguousMemory failed\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(p, PAGE_SIZE);
	*(PUINT32)p = revId & 0x7FFFFFFF; // VMCS Revision ID，bit31=0

	*ppVirt = p;
	*pPhys = MmGetPhysicalAddress(p).QuadPart;
	DbgPrint("[VMX] Region: virt=0x%p phys=0x%llX vmxRevId=0x%X\n", p, *pPhys, revId);
	return STATUS_SUCCESS;
}

// -------------------------------------------------------
// VMWRITE 辅助宏
// -------------------------------------------------------
#define VMWRITE(field, value) \
    do { \
        ULONG_PTR _v = (ULONG_PTR)(value); \
        if (AsmVmWrite((ULONG_PTR)(field), _v) != 0) { \
            DbgPrint("[VMX] VMWRITE failed: field=0x%04X value=0x%llX\n", (UINT32)(field), (UINT64)_v); \
            return STATUS_UNSUCCESSFUL; \
        } \
    } while(0)

// -------------------------------------------------------
// 步骤 3：写入所有必要的 VMCS 字段
// -------------------------------------------------------
static NTSTATUS SetupVmcs(PVOID hostStackTop, UINT64 msrBitmapPhys) {
	UINT64  vmxBasicLocal;
	UINT32  pinMsrIndex, procMsrIndex, exitMsrIndex, entryMsrIndex;
	UINT32  pinCtrl, procCtrl, proc2Ctrl, exitCtrl, entryCtrl;
	ULONG_PTR cr0, cr3, cr4, cr0Guest, cr4Guest;
	UINT64  gdtBase, idtBase;
	UINT16  gdtLimit, idtLimit;
	UINT16  cs, ss, ds, es, fs, gs, tr, ldtr;

	gdtBase = AsmGetGdtrBase();
	gdtLimit = AsmGetGdtrLimit();
	idtBase = AsmGetIdtrBase();
	idtLimit = AsmGetIdtrLimit();
	cs = AsmGetCs();
	ss = AsmGetSs();
	ds = AsmGetDs();
	es = AsmGetEs();
	fs = AsmGetFs();
	gs = AsmGetGs();
	tr = AsmGetTr();
	ldtr = AsmGetLdtr();

	DbgPrint("[VMX] CS=0x%X SS=0x%X DS=0x%X TR=0x%X LDTR=0x%X\n", cs, ss, ds, tr, ldtr);
	DbgPrint("[VMX] GDT base=0x%llX limit=0x%X\n", gdtBase, gdtLimit);
	DbgPrint("[VMX] IDT base=0x%llX limit=0x%X\n", idtBase, idtLimit);

	// --- VMX 控制字段（按 MSR allowed 位调整）---
	// 当 VMX_BASIC bit55 = 1 时，必须使用 TRUE MSR 变体。
	// 非 TRUE MSR 存在遗留"default-1"位（例如 ENTRY_CTLS bit12 = Entry to SMM），
	// 设置这些位会导致 VM-entry 失败（exit reason 33）。
	vmxBasicLocal = __readmsr(MSR_IA32_VMX_BASIC);
	// 这里检查 VMX_BASIC_TRUE_CTLS_BIT 标志位,有两个原因
	// 1:确保使用和硬件能力一致的索引
	// 2:在VMware中,非TRUE标志可能会有问题
	if (vmxBasicLocal & VMX_BASIC_TRUE_CTLS_BIT) {
		pinMsrIndex = TRUE_PIN_CTRL_MSR;
		procMsrIndex = TRUE_PROC_CTRL_MSR;
		exitMsrIndex = TRUE_EXIT_CTRL_MSR;
		entryMsrIndex = TRUE_ENTRY_CTRL_MSR;
		DbgPrint("[VMX] SetupVmcs: VMX_BASIC bit55=1, using TRUE MSRs\n");
	}
	else {
		pinMsrIndex = PIN_CTRL_MSR;
		procMsrIndex = PROC_CTRL_MSR;
		exitMsrIndex = EXIT_CTRL_MSR;
		entryMsrIndex = ENTRY_CTRL_MSR;
		DbgPrint("[VMX] SetupVmcs: VMX_BASIC bit55=0, using non-TRUE MSRs\n");
	}

	// 这里不请求任何额外的pinMsr特性。
	pinCtrl = AdjustCtrl(0, pinMsrIndex);
	// 请求"Use MSR bitmaps"（bit28）和"Activate secondary controls"（bit31）。
	// AdjustCtrl 仅在 TRUE_PROC_CTRL_MSR 允许（may-be-1）时保留这些位。
	// SimpleVisor 明确请求这两位；此处保持一致。
	procCtrl = AdjustCtrl((1u << 28) | (1u << 31), procMsrIndex);
	// 仅在 bit31 实际被允许时才配置 Secondary Proc Controls
	if (procCtrl & (1u << 31)) proc2Ctrl = AdjustCtrl(0, PROC_CTRL2_MSR);
	else proc2Ctrl = 0;
	// VMEXIT_CTRL_HOST_ADDR_SPACE 标志表示请求 Guest 退出后 Host 以64位模式执行（HOST_RIP 是64位代码）
	exitCtrl = AdjustCtrl(VMEXIT_CTRL_HOST_ADDR_SPACE, exitMsrIndex);
	// VMENTRY_CTRL_IA32E_GUEST 标志表示请求 Host 退出后 Guest 以64位模式执行（GuestCodeEntry是64 位代码）
	// 同时要求 GUEST_IA32_EFER.LMA=1（SDM §26.3.1.1），即使不开启 "load EFER" entry control
	entryCtrl = AdjustCtrl(VMENTRY_CTRL_IA32E_GUEST, entryMsrIndex);

	DbgPrint("[VMX] Controls: pin=0x%X proc=0x%X proc2=0x%X exit=0x%X entry=0x%X\n",
		pinCtrl, procCtrl, proc2Ctrl, exitCtrl, entryCtrl);

	VMWRITE(VMCS_CTRL_PIN_EXEC, pinCtrl);
	VMWRITE(VMCS_CTRL_PROC_EXEC, procCtrl);
	VMWRITE(VMCS_CTRL_PROC_EXEC2, proc2Ctrl);
	VMWRITE(VMCS_CTRL_VMEXIT_CTRL, exitCtrl);
	VMWRITE(VMCS_CTRL_VMENTRY_CTRL, entryCtrl);
	VMWRITE(VMCS_CTRL_EXCEPTION_BITMAP, 0);
	VMWRITE(VMCS_CTRL_VMEXIT_MSR_STORE_COUNT, 0);
	VMWRITE(VMCS_CTRL_VMEXIT_MSR_LOAD_COUNT, 0);
	VMWRITE(VMCS_CTRL_VMENTRY_MSR_LOAD_COUNT, 0);
	VMWRITE(VMCS_CTRL_VMENTRY_INTR_INFO, 0);
	VMWRITE(VMCS_GUEST_VMCS_LINK_PTR, (ULONG_PTR)~0ULL); // 禁用 VMCS shadowing

	// 若"Use MSR Bitmaps"（bit28）已设置，则在这里配置 MSR Bitmap 的物理地址。
	// VMCLEAR 会将该字段清零，地址为零在 VMware nested VT-x 校验中会失败。
	// MSR Bitmap 内容全部为 0，表示 Guest 执行任何 RDMSR/WRMSR 指令时均不触发 VM Exit。
	if (procCtrl & (1u << 28)) {
		DbgPrint("[VMX] procCtrl bit28=1 (Use MSR Bitmaps), Set MSR Bitmap PhysAddr=0x%llX\n", msrBitmapPhys);
		VMWRITE(VMCS_CTRL_MSR_BITMAP, msrBitmapPhys);
	}
	else {
		DbgPrint("[VMX] procCtrl bit28=0 (MSR bitmaps not active)\n");
	}


	// --- CR 寄存器 ---
	// Host CR0/CR4：直接读取当前值，OS 运行期间已满足 VMX FIXED 约束，无需调整
	cr0 = __readcr0();
	cr3 = __readcr3();
	cr4 = __readcr4();
	// Guest CR0：按 FIXED0/FIXED1 约束调整，确保 Guest 满足 VMX 要求
	cr0Guest = AdjustCr0(cr0);
	// Guest CR4：先清除 VMXE（bit13），再按 FIXED0/FIXED1 约束调整。
	// Guest 运行在 VMX non-root 模式，不需要 VMX 能力。
	cr4Guest = AdjustCr4(cr4 & ~(1UL << 13));
	DbgPrint("[VMX] CR0(host)=0x%llX CR3=0x%llX CR4(host)=0x%llX\n",
		(UINT64)cr0, (UINT64)cr3, (UINT64)cr4);
	DbgPrint("[VMX] CR0(guest)=0x%llX CR4(guest)=0x%llX\n",
		(UINT64)cr0Guest, (UINT64)cr4Guest);

	// --- Host 状态 ---
	VMWRITE(VMCS_HOST_CR0, cr0);
	VMWRITE(VMCS_HOST_CR3, cr3);
	VMWRITE(VMCS_HOST_CR4, cr4);
	VMWRITE(VMCS_HOST_CS_SEL, cs & 0xF8);
	VMWRITE(VMCS_HOST_DS_SEL, ds & 0xF8);
	VMWRITE(VMCS_HOST_ES_SEL, es & 0xF8);
	VMWRITE(VMCS_HOST_FS_SEL, fs & 0xF8);
	VMWRITE(VMCS_HOST_GS_SEL, gs & 0xF8);
	VMWRITE(VMCS_HOST_SS_SEL, ss & 0xF8);
	VMWRITE(VMCS_HOST_TR_SEL, tr & 0xF8);
	// FS/GS 基地址必须从 MSR 读取，而非从 GDT 解析。
	// 64 位长模式下 GDT 描述符的 Base 字段仅 32 位，已被 CPU 忽略。
	// 实际基地址存放在 IA32_FS_BASE(0xC0000100) / IA32_GS_BASE(0xC0000101)。
	// GS 通常指向 KPCR，其地址为内核虚拟地址，超过 32 位范围。
	VMWRITE(VMCS_HOST_FS_BASE, __readmsr(MSR_IA32_FS_BASE));
	VMWRITE(VMCS_HOST_GS_BASE, __readmsr(MSR_IA32_GS_BASE));
	VMWRITE(VMCS_HOST_TR_BASE, GetSegmentBase(gdtBase, tr));
	VMWRITE(VMCS_HOST_GDTR_BASE, gdtBase);
	VMWRITE(VMCS_HOST_IDTR_BASE, idtBase);
	// 配置VMCS中与系统调用指令sysenter相关的寄存器信息，这里没配置与syscall相关的MSR寄存器
	VMWRITE(VMCS_HOST_IA32_SYSENTER_CS, __readmsr(MSR_IA32_SYSENTER_CS));
	VMWRITE(VMCS_HOST_IA32_SYSENTER_ESP, __readmsr(MSR_IA32_SYSENTER_ESP));
	VMWRITE(VMCS_HOST_IA32_SYSENTER_EIP, __readmsr(MSR_IA32_SYSENTER_EIP));
	// 配置客户机退出时,宿主机要转移到的RSP和RIP
	VMWRITE(VMCS_HOST_RSP, (ULONG_PTR)hostStackTop);
	VMWRITE(VMCS_HOST_RIP, (ULONG_PTR)AsmVmExitHandler);
	// HOST_IA32_PAT：exit controls bit19(Load PAT)=1 时，VM exit 后 CPU 将此字段写入 IA32_PAT MSR。
	// 若字段为 0（VMCS 区域由 AllocVmxRegion 的 RtlZeroMemory 初始化为全 0），Host 所有页变为 UC（不缓存），性能骤降。
	// VMware 嵌套 VT-x + TRUE MSR 通常强制此位为 1，必须显式写入当前 Host 值。
	VMWRITE(VMCS_HOST_IA32_PAT, __readmsr(MSR_IA32_PAT));
	// HOST_IA32_EFER：有两重必要性：
	// 1. exit controls bit21(Load EFER)=1 时，VM exit 后 CPU 将此字段写入 IA32_EFER MSR；
	//    若为 0：LMA=0 → CPU 以 32 位模式执行 64 位代码 → #UD → BSOD；
	//    SCE=0 → SYSCALL 失效；NXE=0 → 内存 NX 保护失效。
	// 2. VM-entry 校验：HOST_ADDR_SPACE=1（64 位 Host）时，LMA/LME 必须为 1，否则 exit reason 33。
	VMWRITE(VMCS_HOST_IA32_EFER, __readmsr(MSR_IA32_EFER));
	DbgPrint("[VMX] HOST_IA32_PAT=0x%llX HOST_IA32_EFER=0x%llX\n", __readmsr(MSR_IA32_PAT), __readmsr(MSR_IA32_EFER));
	DbgPrint("[VMX] Host RSP=0x%p RIP=AsmVmExitHandler=0x%p\n", hostStackTop, AsmVmExitHandler);

	// --- Guest 状态 ---
	VMWRITE(VMCS_GUEST_CR0, cr0Guest); // 按 FIXED 约束调整
	VMWRITE(VMCS_GUEST_CR3, cr3);      // 复用 Host CR3，共享页表
	VMWRITE(VMCS_GUEST_CR4, cr4Guest); // 先清 VMXE 再按 FIXED 调整
	VMWRITE(VMCS_GUEST_DR7, 0x400);
	// CR 读影子：GUEST_HOST_MASK=0 时，Guest 直接读取 CR0/CR4。
	// SimpleVisor 会写这些字段；此处保持一致以确保正确性。
	VMWRITE(VMCS_CTRL_CR0_READ_SHADOW, cr0Guest);
	VMWRITE(VMCS_CTRL_CR4_READ_SHADOW, cr4Guest);

	// 段选择子：复用 Host 段
	VMWRITE(VMCS_GUEST_CS_SEL, cs);
	VMWRITE(VMCS_GUEST_SS_SEL, ss);
	VMWRITE(VMCS_GUEST_DS_SEL, ds);
	VMWRITE(VMCS_GUEST_ES_SEL, es);
	VMWRITE(VMCS_GUEST_FS_SEL, fs);
	VMWRITE(VMCS_GUEST_GS_SEL, gs);
	VMWRITE(VMCS_GUEST_TR_SEL, tr);
	VMWRITE(VMCS_GUEST_LDTR_SEL, ldtr);

	// 段基地址（64 位 long mode 下 CS/SS/DS/ES 基地址为 0）
	VMWRITE(VMCS_GUEST_CS_BASE, 0);
	VMWRITE(VMCS_GUEST_SS_BASE, 0);
	VMWRITE(VMCS_GUEST_DS_BASE, 0);
	VMWRITE(VMCS_GUEST_ES_BASE, 0);
	VMWRITE(VMCS_GUEST_FS_BASE, __readmsr(MSR_IA32_FS_BASE));
	VMWRITE(VMCS_GUEST_GS_BASE, __readmsr(MSR_IA32_GS_BASE));
	VMWRITE(VMCS_GUEST_TR_BASE, GetSegmentBase(gdtBase, tr));
	VMWRITE(VMCS_GUEST_LDTR_BASE, GetSegmentBase(gdtBase, ldtr));
	VMWRITE(VMCS_GUEST_GDTR_BASE, gdtBase);
	VMWRITE(VMCS_GUEST_IDTR_BASE, idtBase);

	// 段界限：从 GDT 实际读取（考虑 G 位展开）。
	// SimpleVisor 使用 __segmentlimit()；此处使用 GetSegmentLimit2()，
	// 读取相同的 GDT 字节。64 位 CS（G=0）：limit=0xFFFF，而非 4GB。
	// VMware nested VT-x 在 G=0 时可能拒绝 4GB 的 limit 值。
	{
		UINT32 csLim = GetSegmentLimit2(gdtBase, cs);
		UINT32 ssLim = GetSegmentLimit2(gdtBase, ss);
		UINT32 dsLim = GetSegmentLimit2(gdtBase, ds);
		UINT32 esLim = GetSegmentLimit2(gdtBase, es);
		UINT32 fsLim = GetSegmentLimit2(gdtBase, fs);
		UINT32 gsLim = GetSegmentLimit2(gdtBase, gs);
		UINT32 trLim = GetSegmentLimit2(gdtBase, tr);
		UINT32 ldLim = GetSegmentLimit2(gdtBase, ldtr);
		DbgPrint("[VMX] Limits: CS=0x%X SS=0x%X DS=0x%X ES=0x%X FS=0x%X GS=0x%X TR=0x%X\n",
			csLim, ssLim, dsLim, esLim, fsLim, gsLim, trLim);
		VMWRITE(VMCS_GUEST_CS_LIMIT, csLim);
		VMWRITE(VMCS_GUEST_SS_LIMIT, ssLim);
		VMWRITE(VMCS_GUEST_DS_LIMIT, dsLim);
		VMWRITE(VMCS_GUEST_ES_LIMIT, esLim);
		VMWRITE(VMCS_GUEST_FS_LIMIT, fsLim);
		VMWRITE(VMCS_GUEST_GS_LIMIT, gsLim);
		VMWRITE(VMCS_GUEST_TR_LIMIT, trLim);
		VMWRITE(VMCS_GUEST_LDTR_LIMIT, ldLim);
		VMWRITE(VMCS_GUEST_GDTR_LIMIT, gdtLimit);
		VMWRITE(VMCS_GUEST_IDTR_LIMIT, idtLimit);
	}

	// 段访问权限（Access Rights）。
	// bits[15:12] 必须为 0（Intel SDM 26.3.1.2 保留位），否则 VM-entry 失败（reason 33）。
	// AR_FROM_LAR 提取：[7:0]=访问字节（type,S,DPL,P），[11:8]=标志(G,D/B,L,AVL)，[15:12]=0
	{
		UINT32 csAr, ssAr, dsAr, esAr, fsAr, gsAr, trAr, ldtrAr;
		csAr = GetSegmentAr(cs);
		ssAr = GetSegmentAr(ss);
		dsAr = GetSegmentAr(ds);
		esAr = GetSegmentAr(es);
		fsAr = GetSegmentAr(fs);
		gsAr = GetSegmentAr(gs);
		trAr = GetSegmentAr(tr);
		ldtrAr = (ldtr == 0) ? AR_UNUSABLE : GetSegmentAr(ldtr);
		DbgPrint("[VMX] AR: CS=0x%X SS=0x%X DS=0x%X ES=0x%X FS=0x%X GS=0x%X TR=0x%X LDTR=0x%X\n",
			csAr, ssAr, dsAr, esAr, fsAr, gsAr, trAr, ldtrAr);
		// 检查：bits[11:8] 对所有段必须为 0（SDM Table 24-2 保留位）。
		// 注意：bits[15:12] = {G,D/B,L,AVL} 不是保留位，是有效标志位。
		if ((csAr & 0x0F00) || (ssAr & 0x0F00) || (dsAr & 0x0F00) || (esAr & 0x0F00) ||
			(fsAr & 0x0F00) || (gsAr & 0x0F00) || (trAr & 0x0F00) ||
			((ldtrAr != AR_UNUSABLE) && (ldtrAr & 0x0F00))) {
			DbgPrint("[VMX] ERROR: segment AR has non-zero reserved bits[11:8]! Will cause exit reason 33.\n");
		}
		VMWRITE(VMCS_GUEST_CS_AR, csAr);
		VMWRITE(VMCS_GUEST_SS_AR, ssAr);
		VMWRITE(VMCS_GUEST_DS_AR, dsAr);
		VMWRITE(VMCS_GUEST_ES_AR, esAr);
		VMWRITE(VMCS_GUEST_FS_AR, fsAr);
		VMWRITE(VMCS_GUEST_GS_AR, gsAr);
		VMWRITE(VMCS_GUEST_TR_AR, trAr);
		VMWRITE(VMCS_GUEST_LDTR_AR, ldtrAr);
	}

	// 无中断阻断：Guest 入口时不处于 STI/MOV-SS/SMI/NMI 阻断状态，中断可正常递交。
	VMWRITE(VMCS_GUEST_INTERRUPTIBILITY, 0);
	// Active：Guest 入口后直接执行指令；写 1（HLT）则 Guest 立即挂起，永远不会到达 GuestCodeEntry。
	VMWRITE(VMCS_GUEST_ACTIVITY_STATE, 0);
	// 无挂起调试异常：BS/B0-B3 全清，与 Interruptibility=0 和 RFLAGS.TF=0 保持一致，通过 VM-entry 校验。
	VMWRITE(VMCS_GUEST_PENDING_DBG_EXCEPTIONS, 0);
	// CR3 豁免值计数为 0：本项目未开启 CR3-load exiting，此字段仅需填合法值（>4 导致 VM-entry 失败）。
	VMWRITE(VMCS_CTRL_CR3_TARGET_COUNT, 0);

	VMWRITE(VMCS_GUEST_IA32_SYSENTER_CS, __readmsr(MSR_IA32_SYSENTER_CS));
	VMWRITE(VMCS_GUEST_IA32_SYSENTER_ESP, __readmsr(MSR_IA32_SYSENTER_ESP));
	VMWRITE(VMCS_GUEST_IA32_SYSENTER_EIP, __readmsr(MSR_IA32_SYSENTER_EIP));
	VMWRITE(VMCS_GUEST_IA32_DEBUGCTL, 0);

	// 本项目 Guest 复用 Host 状态，直接写入 Host 当前值。
	{
		UINT64 efMsr = __readmsr(MSR_IA32_EFER);
		UINT64 patMsr = __readmsr(MSR_IA32_PAT);
		// GUEST_IA32_EFER：SDM §26.3.1.1 强制要求：
		//   "IA-32e mode guest"=1 且 CR0.PG=1 时，EFER.LMA 必须为 1。
		//   VMCS 区域由 AllocVmxRegion 的 RtlZeroMemory 初始化为全 0，
		//   若不显式写入则 LMA=0，VM-entry 失败（exit reason 33）。
		VMWRITE(VMCS_GUEST_IA32_EFER, efMsr);
		// GUEST_IA32_PAT：entry controls bit13(Load PAT)=1 时，
		//   VM entry 后 CPU 将此字段值加载到 IA32_PAT MSR，若为 0 则 Guest 缓存配置异常。
		VMWRITE(VMCS_GUEST_IA32_PAT, patMsr);
		DbgPrint("[VMX] IA32_EFER=0x%llX (LME=%llu LMA=%llu SCE=%llu NXE=%llu)\n",
			efMsr, (efMsr >> 8) & 1, (efMsr >> 10) & 1, efMsr & 1, (efMsr >> 11) & 1);
		DbgPrint("[VMX] IA32_PAT=0x%llX\n", patMsr);
	}

	// Guest 代码执行入口
	VMWRITE(VMCS_GUEST_RSP, ((ULONG_PTR)hostStackTop - 0x800) & ~0xFULL);
	VMWRITE(VMCS_GUEST_RIP, (ULONG_PTR)AsmGuestCodeEntry);
	VMWRITE(VMCS_GUEST_RFLAGS, 0x2); // 必须位，IF=0,禁止中断

	DbgPrint("[VMX] Guest RIP=AsmGuestCodeEntry=0x%p\n", AsmGuestCodeEntry);
	DbgPrint("[VMX] VMCS setup complete\n");
	return STATUS_SUCCESS;
}

// -------------------------------------------------------
// VM Exit 处理函数（由 AsmVmExitHandler 调用）
// guestRax：Guest 退出时的 RAX 值（= A+B 的计算结果）
// -------------------------------------------------------
VOID VmExitHandler(UINT64 guestRax) {
	UINT64 exitReason = AsmVmRead(VMCS_RO_EXIT_REASON) & 0xFFFF;
	UINT64 intrInfo = AsmVmRead(VMCS_RO_VM_EXIT_INTR_INFO);
	UINT64 qual = AsmVmRead(VMCS_RO_EXIT_QUALIFICATION);

	DbgPrint("[VMX] VM Exit: reason=%llu qual=0x%llX intrInfo=0x%llX\n", exitReason, qual, intrInfo);
	if (exitReason == EXIT_REASON_VMCALL) {
		ULONG_PTR guestRIP = AsmVmRead(VMCS_GUEST_RIP);
		AsmVmWrite(VMCS_GUEST_RIP, guestRIP + 3);
		g_GuestResult = guestRax;
		g_VmRunOccurred = TRUE;
		DbgPrint("[VMX] VMCALL Exit OK, Guest RAX = %llu\n", guestRax);
	}
	else {
		UINT64 instrErr = AsmVmRead(VMCS_RO_VM_INSTRUCTION_ERROR);
		UINT64 guestRip = AsmVmRead(VMCS_GUEST_RIP);
		DbgPrint("[VMX] Unexpected exit! instrErr=0x%llX guestRip=0x%llX\n", instrErr, guestRip);
		// 解析 intrInfo，处理异常/NMI（reason 33）
		if ((intrInfo >> 31) & 1) {
			UINT64 vector = intrInfo & 0xFF;
			UINT64 type = (intrInfo >> 8) & 0x7;
			UINT64 hasErr = (intrInfo >> 11) & 0x1;
			UINT64 errCode = hasErr ? AsmVmRead(VMCS_RO_VM_EXIT_INTR_ERROR_CODE) : 0;
			DbgPrint("[VMX] IntrInfo valid: vector=%llu type=%llu hasErrCode=%llu errCode=0x%llX\n", vector, type, hasErr, errCode);
			DbgPrint("[VMX]   type: 0=ExtInt 2=NMI 3=HWexception 6=SWexception\n");
			DbgPrint("[VMX]   common vectors: 0=#DE 6=#UD 8=#DF 13=#GP 14=#PF\n");
		}
	}
}


// ========================================================
// RunVmxTest：完整独立的虚拟化测试函数
// 职责：分配所有 VMX 资源 → 执行完整 VMX 生命周期 → 释放所有资源
// 调用者无需关心任何 VMX 细节，只需传入 a、b，读取 result。
// ========================================================
NTSTATUS RunVmxTest(UINT64 addA, UINT64 addB, UINT64 *pResult) {
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	KAFFINITY oldThreadAffinity;
	ULONG currentCpu = 0;
	UINT64 vmxBasic = 0;
	UINT32 vmxRevId = 0;
	ULONG_PTR cr4 = 0;
	PVOID vmxonVirt, vmcsVirt, msrBitmapVirt;
	UINT64 vmxonPhys, vmcsPhys, msrBitmapPhys;
	BOOLEAN vmxActive = FALSE;
	BOOLEAN vmcsIsBind = FALSE;
	PVOID hostStackVirt, hostStackTop;
	UINT8 ret;
	UINT64 expectedVal = 0;

	vmxonVirt = NULL;
	vmcsVirt = NULL;
	msrBitmapVirt = NULL;
	vmxonPhys = 0;
	vmcsPhys = 0;
	msrBitmapPhys = 0;
	hostStackVirt = NULL;
	hostStackTop = NULL;


	DbgPrint("[VMX] ===== VMX Test Start =====\n");
	currentCpu = KeGetCurrentProcessorNumber();
	DbgPrint("[VMX] Starting on CPU %lu\n", currentCpu);
	oldThreadAffinity = KeSetSystemAffinityThreadEx((KAFFINITY)(1ULL << currentCpu));

	// 1. 检查 VMX 支持
	// 如果 CPU 不支持 VMX，直接读 MSR_IA32_VMX_BASIC 会触发 #GP（BSOD）
	status = CheckVmxSupport();
	if (!NT_SUCCESS(status)) {
		DbgPrint("[VMX] VMX not supported on this CPU, aborting\n");
		goto Cleanup;
	}

	// 2. 读取 VMCS Revision ID
	vmxBasic = __readmsr(MSR_IA32_VMX_BASIC);
	vmxRevId = (UINT32)(vmxBasic & 0x7FFFFFFF);
	DbgPrint("[VMX] IA32_VMX_BASIC=0x%llX RevId=0x%X\n", vmxBasic, vmxRevId);

	// 3. 分配 VMXON 区域（4KB 物理连续）
	status = AllocVmxRegion(&vmxonVirt, &vmxonPhys, vmxRevId);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[VMX] VMXON region alloc failed\n");
		goto Cleanup;
	}
	// 4. 分配 VMCS 区域（4KB 物理连续）
	status = AllocVmxRegion(&vmcsVirt, &vmcsPhys, vmxRevId);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[VMX] VMCS region alloc failed\n");
		goto Cleanup;
	}

	// 5. 分配 Host 栈
	hostStackVirt = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, 'xmvH');
	if (!hostStackVirt) {
		DbgPrint("[VMX] Host stack alloc failed\n");
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}
	RtlZeroMemory(hostStackVirt, PAGE_SIZE);
	hostStackTop = (PUINT8)hostStackVirt + PAGE_SIZE - 16; // 栈向低地址增长，16 字节对齐
	DbgPrint("[VMX] Host stack: base=0x%p top=0x%p\n", hostStackVirt, hostStackTop);

	// 6. 分配 MSR bitmap（4KB 全零 = 不触发任何 MSR VM exit）。
	// 当"use MSR bitmaps" proc control（bit28）被强制开启时需要提供有效地址。
	// VMware nested VT-x 强制开启此位并校验 bitmap 地址。
	status = AllocVmxRegion(&msrBitmapVirt, &msrBitmapPhys, 0 /* vmxRevId=0 keeps page zero */);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[VMX] MSR bitmap alloc failed\n");
		goto Cleanup;
	}
	DbgPrint("[VMX] MSR bitmap: virt=0x%p phys=0x%llX (all-zero)\n", msrBitmapVirt, msrBitmapPhys);

	// 7a. 设置 CR4.VMXE 标志
	cr4 = __readcr4() | (1UL << 13);
	__writecr4(cr4);
	DbgPrint("[VMX] CR4.VMXE set, CR4=0x%llX\n", (UINT64)cr4);

	// 7b. 为当前CPU绑定VMXON控制块(汇编指令:vmxon)
	DbgPrint("[VMX] Executing VMXON (phys=0x%llX)...\n", vmxonPhys);
	ret = AsmVmxOn(&vmxonPhys);
	if (ret != 0) {
		DbgPrint("[VMX] VMXON instruction failed: ret=%d (1=CF 2=ZF)\n", ret);
		DbgPrint("[VMX]   CF: invalid VMXON region; ZF: already in VMX root or constraints\n");
		goto Cleanup;
	}
	vmxActive = TRUE;
	DbgPrint("[VMX] VMXON success - VMX root mode active\n");

	// 7c. VMCLEAR
	DbgPrint("[VMX] Executing VMCLEAR (phys=0x%llX)...\n", vmcsPhys);
	ret = AsmVmClear(&vmcsPhys);
	if (ret != 0) {
		DbgPrint("[VMX] VMCLEAR failed, code=%d\n", ret);
		goto Cleanup;
	}
	DbgPrint("[VMX] VMCLEAR success\n");

	// 8. 为当前CPU绑定VMCS控制块(汇编指令:vmptrld)
	DbgPrint("[VMX] [CP8] Executing VMPTRLD...\n");
	ret = AsmVmPtrld(&vmcsPhys);
	if (ret != 0) {
		DbgPrint("[VMX] VMPTRLD failed, code=%d\n", ret);
		goto Cleanup;
	}
	vmcsIsBind = TRUE;
	DbgPrint("[VMX] VMPTRLD success - VMCS active\n");

	// 9. 初始化 VMCS
	status = SetupVmcs(hostStackTop, msrBitmapPhys);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[VMX] VMCS setup failed\n");
		goto Cleanup;
	}

	// 10. 执行 VMLAUNCH & VMRESUME
	// RCX=addA, RDX=addB 成为 Guest 初始寄存器值。
	// VMLAUNCH 成功：VM exit -> AsmVmExitHandler -> g_HostRsp -> 返回此处
	// VMLAUNCH 失败：AsmVmRun 直接返回
	DbgPrint("[VMX] Guest input: A=%llu B=%llu expected=%llu\n", addA, addB, addA + addB);
	expectedVal = addA + addB + 2;
	DbgPrint("[VMX] Executing VMLAUNCH (RCX=%llu RDX=%llu),GuestRIP=%llx\n", addA, addB , AsmVmRead(VMCS_GUEST_RIP));
	AsmVmRun(addA, addB);
	DbgPrint("[VMX] Executed VMLAUNCH Reuslt=%llu GuestRIP=%llx\n", g_GuestResult, AsmVmRead(VMCS_GUEST_RIP));
	AsmVmRun(g_GuestResult, 1);
	DbgPrint("[VMX] Executed VMRESUME Reuslt=%llu GuestRIP=%llx\n", g_GuestResult, AsmVmRead(VMCS_GUEST_RIP));
	AsmVmRun(g_GuestResult, 1);
	DbgPrint("[VMX] Executed VMRESUME Reuslt=%llu GuestRIP=%llx\n", g_GuestResult, AsmVmRead(VMCS_GUEST_RIP));
	*pResult = g_GuestResult;

	// 此处 VMX 仍然处于激活状态（VmExitHandler 不再调用 VMXOFF）。
	// 现在可以安全调用 AsmVmRead/AsmVmWrite。
	if (!g_VmRunOccurred) {
		UINT64 errCode = AsmVmRead(VMCS_RO_VM_INSTRUCTION_ERROR);
		DbgPrint("[VMX] VMLAUNCH returned without VMCALL exit!\n");
		DbgPrint("[VMX] VM Instruction Error=0x%llX (Table 30-1 in Intel SDM Vol.3C)\n", errCode);
		goto Cleanup;
	}

	DbgPrint("[VMX] %llu + %llu + 1 + 1 = %llu (computed by Guest)\n", addA, addB, g_GuestResult);
	if (g_GuestResult == expectedVal) {
		DbgPrint("[VMX] Result correct! Intel VT-x verified.\n");
		status = STATUS_SUCCESS;
	}
	else {
		DbgPrint("[VMX] Result wrong! expected=%llu got=%llu\n", expectedVal, g_GuestResult);
	}

Cleanup:
	if (vmcsIsBind) {
		// 解除VMCS控制块和CPU的绑定关系
		// VMCLEAR 将已启动的 VMCS 从硬件缓存刷回内存，并将其标记为非活动状态。
		// CPU 可能将过期的 VMCS 数据写回已释放（并可能被复用）的物理页，导致内存损坏进而引发系统冻结。
		// 在本程序中这不是必须的,因为后续调用了AsmVmxOff,关闭了vmx功能,VMCS控制块不会被刷回内存
		// 在本程序中这不是必须的：后续的 VMXOFF 退出 VMX 模式后，
		// CPU 不再有任何机制将 VMCS 缓存写回物理页，释放内存是安全的。
		// 保留 VMCLEAR 是为了遵循 SDM §24.11.3 推荐的标准流程。
		AsmVmClear(&vmcsPhys);
	}
	if (vmxActive) {
		// 让CPU 退出 VMX 模式
		AsmVmxOff();
		DbgPrint("[VMX] Cleanup: VMXOFF\n");
	}

	// VMXOFF 后清除 CR4.VMXE
	cr4 = __readcr4();
	__writecr4(cr4 & ~(1UL << 13));

	KeRevertToUserAffinityThreadEx(oldThreadAffinity);

	if (hostStackVirt) ExFreePool(hostStackVirt);
	if (msrBitmapVirt) MmFreeContiguousMemory(msrBitmapVirt);
	if (vmcsVirt) MmFreeContiguousMemory(vmcsVirt);
	if (vmxonVirt) MmFreeContiguousMemory(vmxonVirt);

	DbgPrint("[VMX] RunVmxTest exit, status=0x%X\n", status);
	// 返回失败以使驱动在执行完毕后立即卸载
	return status;
}

static NTSTATUS VmxIrpCreate(PDEVICE_OBJECT DevObj, PIRP Irp) {
	UNREFERENCED_PARAMETER(DevObj);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

static NTSTATUS VmxIrpClose(PDEVICE_OBJECT DevObj, PIRP Irp) {
	UNREFERENCED_PARAMETER(DevObj);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

static NTSTATUS VmxIrpDeviceControl(PDEVICE_OBJECT DevObj, PIRP Irp) {
	PIO_STACK_LOCATION stack;
	ULONG     code, inLen, outLen;
	PVOID     buf;
	NTSTATUS  status = STATUS_INVALID_DEVICE_REQUEST;
	ULONG_PTR info = 0;

	UNREFERENCED_PARAMETER(DevObj);

	stack = IoGetCurrentIrpStackLocation(Irp);
	code = stack->Parameters.DeviceIoControl.IoControlCode;
	inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
	outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
	buf = Irp->AssociatedIrp.SystemBuffer;

	if (code == IOCTL_HVLAB_RUN_GUEST) {
		if (inLen < sizeof(HVLAB_RUN_GUEST_INPUT) || outLen < sizeof(HVLAB_RUN_GUEST_OUTPUT)) {
			status = STATUS_BUFFER_TOO_SMALL;
		}

		HVLAB_RUN_GUEST_INPUT  *in = (HVLAB_RUN_GUEST_INPUT *)buf;
		HVLAB_RUN_GUEST_OUTPUT *out = (HVLAB_RUN_GUEST_OUTPUT *)buf;
		UINT64 result = ~0ULL;
		// 串行化：确保同一时刻只有一个 VMX 实验在运行
		KeWaitForSingleObject(&g_VmxMutex, Executive, KernelMode, FALSE, NULL);
		status = RunVmxTest(in->a, in->b, &result);
		KeReleaseMutex(&g_VmxMutex, FALSE);

		if (NT_SUCCESS(status)) {
			out->result = result;
			info = sizeof(HVLAB_RUN_GUEST_OUTPUT);
		}

	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

static VOID VmxDriverUnload(PDRIVER_OBJECT driverObject) {
	UNICODE_STRING symLink;
	UNREFERENCED_PARAMETER(driverObject);

	if (g_SymLinkCreated) {
		RtlInitUnicodeString(&symLink, HVLAB_SYMLINK_NAME);
		IoDeleteSymbolicLink(&symLink);
	}
	if (g_DeviceObject) {
		IoDeleteDevice(g_DeviceObject);
	}
	DbgPrint("[VMX] Driver Unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
	NTSTATUS       status;
	UNICODE_STRING devName, symName;

	UNREFERENCED_PARAMETER(registryPath);

	DbgPrint("[VMX] VmxLab loading...\n");

	// 创建设备对象
	RtlInitUnicodeString(&devName, HVLAB_DEVICE_NAME);
	status = IoCreateDevice(driverObject, 0, &devName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &g_DeviceObject);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[VMX] IoCreateDevice failed: 0x%X\n", status);
		return status;
	}
	g_DeviceObject->Flags |= DO_BUFFERED_IO;
	g_DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	// 创建符号链接（用户态通过 \\.\VmxLab 访问）
	RtlInitUnicodeString(&symName, HVLAB_SYMLINK_NAME);
	status = IoCreateSymbolicLink(&symName, &devName);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[VMX] IoCreateSymbolicLink failed: 0x%X\n", status);
		IoDeleteDevice(g_DeviceObject);
		return status;
	}
	g_SymLinkCreated = TRUE;

	// 注册 IRP 处理函数与卸载回调
	KeInitializeMutex(&g_VmxMutex, 0);
	driverObject->MajorFunction[IRP_MJ_CREATE] = VmxIrpCreate;
	driverObject->MajorFunction[IRP_MJ_CLOSE] = VmxIrpClose;
	driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = VmxIrpDeviceControl;
	driverObject->DriverUnload = VmxDriverUnload;

	DbgPrint("[VMX] Driver ready. Use VmxLabClient.exe to run VMX tests.\n");
	return STATUS_SUCCESS;
}

; VmxLab author:flysnowxg email:308821698@qq.com date:20251205
; driver_asm.asm - VmxLab, 64-bit MASM, VMX instructions + segment register helpers + Guest code

EXTERN VmExitHandler : PROC
EXTERN g_HostRsp     : QWORD
EXTERN g_VmLaunched  : BYTE

.CODE

; -------------------------------------------------------
; UINT8 AsmVmxOn(UINT64* pPhysAddr)
; Return: 0=success, 1=CF, 2=ZF
; -------------------------------------------------------
AsmVmxOn PROC
    vmxon QWORD PTR [rcx]
    jc    vmxon_cf
    jz    vmxon_zf
    xor   eax, eax
    ret
vmxon_cf:
    mov   eax, 1
    ret
vmxon_zf:
    mov   eax, 2
    ret
AsmVmxOn ENDP

; -------------------------------------------------------
; VOID AsmVmxOff(VOID)
; -------------------------------------------------------
AsmVmxOff PROC
    vmxoff
    ret
AsmVmxOff ENDP

; -------------------------------------------------------
; UINT8 AsmVmClear(UINT64* pPhysAddr)
; -------------------------------------------------------
AsmVmClear PROC
    vmclear QWORD PTR [rcx]
    jc    vmclear_cf
    jz    vmclear_zf
    xor   eax, eax
    ret
vmclear_cf:
    mov   eax, 1
    ret
vmclear_zf:
    mov   eax, 2
    ret
AsmVmClear ENDP

; -------------------------------------------------------
; UINT8 AsmVmPtrld(UINT64* pPhysAddr)
; -------------------------------------------------------
AsmVmPtrld PROC
    vmptrld QWORD PTR [rcx]
    jc    vmptrld_cf
    jz    vmptrld_zf
    xor   eax, eax
    ret
vmptrld_cf:
    mov   eax, 1
    ret
vmptrld_zf:
    mov   eax, 2
    ret
AsmVmPtrld ENDP

; -------------------------------------------------------
; UINT8 AsmVmWrite(ULONG_PTR field, ULONG_PTR value)
; RCX=field, RDX=value
; -------------------------------------------------------
AsmVmWrite PROC
    vmwrite rcx, rdx
    jc    vmwrite_cf
    jz    vmwrite_zf
    xor   eax, eax
    ret
vmwrite_cf:
    mov   eax, 1
    ret
vmwrite_zf:
    mov   eax, 2
    ret
AsmVmWrite ENDP

; -------------------------------------------------------
; ULONG_PTR AsmVmRead(ULONG_PTR field)
; RCX=field, return RAX=value
; -------------------------------------------------------
AsmVmRead PROC
    vmread rax, rcx
    ret
AsmVmRead ENDP

; -------------------------------------------------------
; Segment register readers (x64 MSVC has no inline asm)
; -------------------------------------------------------
AsmGetCs PROC
    mov   ax, cs
    ret
AsmGetCs ENDP

AsmGetSs PROC
    mov   ax, ss
    ret
AsmGetSs ENDP

AsmGetDs PROC
    mov   ax, ds
    ret
AsmGetDs ENDP

AsmGetEs PROC
    mov   ax, es
    ret
AsmGetEs ENDP

AsmGetFs PROC
    mov   ax, fs
    ret
AsmGetFs ENDP

AsmGetGs PROC
    mov   ax, gs
    ret
AsmGetGs ENDP

AsmGetTr PROC
    str   ax
    ret
AsmGetTr ENDP

AsmGetLdtr PROC
    sldt  ax
    ret
AsmGetLdtr ENDP

; -------------------------------------------------------
; UINT64 AsmGetGdtrBase(VOID)
; In x64: GDTR = 2-byte limit + 8-byte base (10 bytes total)
; -------------------------------------------------------
AsmGetGdtrBase PROC
    sub   rsp, 16
    sgdt  [rsp]
    mov   rax, QWORD PTR [rsp+2]
    add   rsp, 16
    ret
AsmGetGdtrBase ENDP

; -------------------------------------------------------
; UINT16 AsmGetGdtrLimit(VOID)
; -------------------------------------------------------
AsmGetGdtrLimit PROC
    sub   rsp, 16
    sgdt  [rsp]
    movzx eax, WORD PTR [rsp]
    add   rsp, 16
    ret
AsmGetGdtrLimit ENDP

; -------------------------------------------------------
; UINT64 AsmGetIdtrBase(VOID)
; -------------------------------------------------------
AsmGetIdtrBase PROC
    sub   rsp, 16
    sidt  [rsp]
    mov   rax, QWORD PTR [rsp+2]
    add   rsp, 16
    ret
AsmGetIdtrBase ENDP

; -------------------------------------------------------
; UINT16 AsmGetIdtrLimit(VOID)
; -------------------------------------------------------
AsmGetIdtrLimit PROC
    sub   rsp, 16
    sidt  [rsp]
    movzx eax, WORD PTR [rsp]
    add   rsp, 16
    ret
AsmGetIdtrLimit ENDP

; -------------------------------------------------------
; UINT32 AsmGetLar(UINT16 selector)   RCX=selector
; -------------------------------------------------------
AsmGetLar PROC
    lar   eax, ecx
    ret
AsmGetLar ENDP

; -------------------------------------------------------
; VOID AsmVmLaunch(UINT64 rcxVal, UINT64 rdxVal)
;
; Save non-volatile regs and current RSP to g_HostRsp,
; then execute VMLAUNCH with RCX/RDX as Guest initial values.
;
; On VMLAUNCH success: this function never returns normally.
;   VM-exit goes to VmExitHandlerAsm which restores g_HostRsp and rets.
; On VMLAUNCH failure (CF/ZF set): pop saved regs and return normally.
; -------------------------------------------------------
AsmVmRun PROC
    push  rbx
    push  rdi
    push  rsi
    push  r12
    push  r13
    push  r14
    push  r15
    push  rbp
    ; Save RFLAGS (IF=1 here, kernel PASSIVE_LEVEL).
    ; After a VM exit, the CPU resets RFLAGS to 0x2 (IF=0).
    ; AsmVmExitHandler will popfq to restore IF=1 before returning.
    pushfq

    ; Save RSP so VmExitHandlerAsm can restore the call frame
    mov   QWORD PTR [g_HostRsp], rsp

	cmp BYTE PTR [g_VmLaunched], 0
    je do_vmlaunch
    vmresume
    jmp failure
do_vmlaunch:
	mov BYTE PTR [g_VmLaunched], 1
    ; RCX=rcxVal, RDX=rdxVal are already in the right registers per ABI.
    ; VMLAUNCH will use the current register state as Guest initial registers.
    vmlaunch
failure:
    ; VMLAUNCH failed - restore RFLAGS and non-volatile regs, return normally
    popfq
    pop   rbp
    pop   r15
    pop   r14
    pop   r13
    pop   r12
    pop   rsi
    pop   rdi
    pop   rbx
    ret
AsmVmRun ENDP

; -------------------------------------------------------
; VmExitHandlerAsm - VMCS HOST_RIP points here
;
; On VM-exit: CPU loads Host state from VMCS.
;   RSP = VMCS HOST_RSP (our clean host stack)
;   RIP = VmExitHandlerAsm
;   RAX = Guest RAX at exit (= A+B computed by guest)
;
; Call VmExitHandler(guestRax), then restore g_HostRsp to
; unwind back to the DriverEntry caller of AsmVmLaunch.
; -------------------------------------------------------
AsmVmExitHandler PROC
    ; Build shadow space per Windows x64 ABI
    sub   rsp, 28h

    ; Pass Guest RAX as first argument
    mov   rcx, rax
    call  VmExitHandler

    add   rsp, 28h

    ; Restore the RSP saved by AsmVmLaunch
    mov   rsp, QWORD PTR [g_HostRsp]

    ; Restore RFLAGS saved by AsmVmLaunch (re-enables interrupts, IF=1).
    ; VM exit always resets RFLAGS to 0x2 (IF=0); without this popfq,
    ; DriverEntry would return to Windows with interrupts disabled,
    ; causing VMware to spin at 100% CPU trying to inject timer interrupts.
    popfq

    ; Pop saved non-volatile registers (mirroring AsmVmLaunch pushes)
    pop   rbp
    pop   r15
    pop   r14
    pop   r13
    pop   r12
    pop   rsi
    pop   rdi
    pop   rbx

    ; Return to DriverEntry (past the AsmVmLaunch call site)
    ret
AsmVmExitHandler ENDP

; -------------------------------------------------------
; GuestCodeEntry - VMCS GUEST_RIP points here
;
; Guest initial state: RCX=A, RDX=B (set before VMLAUNCH)
; Compute RAX = RCX + RDX, then VMCALL to exit.
; -------------------------------------------------------
AsmGuestCodeEntry PROC
start:
    mov   rax, rcx
    add   rax, rdx
    vmcall
	jmp	  start
    hlt             ; should not reach here
AsmGuestCodeEntry ENDP

END

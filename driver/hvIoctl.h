// VmxLab author:flysnowxg email:308821698@qq.com date:20251205
#pragma once

#define HVLAB_DEVICE_TYPE  0x8000

// IOCTL: 执行 VMX 生命周期，Guest 计算 a+b+1+1，返回结果
#define IOCTL_HVLAB_RUN_GUEST CTL_CODE(HVLAB_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)

typedef struct _HVLAB_RUN_GUEST_INPUT {
    unsigned __int64 a;
    unsigned __int64 b;
} HVLAB_RUN_GUEST_INPUT;

typedef struct _HVLAB_RUN_GUEST_OUTPUT {
    unsigned __int64 result;
} HVLAB_RUN_GUEST_OUTPUT;

#pragma pack(pop)

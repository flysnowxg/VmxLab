// VmxLab author:flysnowxg email:308821698@qq.com date:20251205
// client.c - VmxLab 用户模式测试程序
// 通过 IOCTL 与内核驱动通信，触发 VMX 实验并读取结果。
// 用法: client.exe <a> <b>

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#define INITGUID 
#include <guiddef.h>
#include "..\driver\Hvioctl.h"

#define HVLAB_DEVICE_NAME  L"\\\\.\\VmxLab"

int wmain(int argc, wchar_t *argv[])
{
	HANDLE hDevice;
	HVLAB_RUN_GUEST_INPUT  input;
	HVLAB_RUN_GUEST_OUTPUT output;
	DWORD bytesReturned;
	BOOL ok;

	if (argc != 3) {
		wprintf(L"Usage: client.exe <a> <b>\n");
		wprintf(L"  Guest VM will Compute Result = a + b via VMX\n");
		return 1;
	}

	input.a = (unsigned __int64)_wcstoui64(argv[1], NULL, 10);
	input.b = (unsigned __int64)_wcstoui64(argv[2], NULL, 10);
	output.result = 0;

	hDevice = CreateFileW(HVLAB_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE,
		0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hDevice == INVALID_HANDLE_VALUE) {
		wprintf(L"[ERROR] Cannot Open Device %s (Error %lu)\n", HVLAB_DEVICE_NAME, GetLastError());
		wprintf(L"        Make Sure The Driver is Loaded: VmxLab.bat start\n");
		return 1;
	}

	ok = DeviceIoControl(hDevice, IOCTL_HVLAB_RUN_GUEST, &input, sizeof(input),
		&output, sizeof(output), &bytesReturned, NULL);

	CloseHandle(hDevice);

	if (!ok) {
		wprintf(L"[ERROR] DeviceIoControl Failed (error %lu)\n", GetLastError());
		return 1;
	}

	wprintf(L"[OK] %llu+%llu+1+1=%llu\n", input.a, input.b, output.result);
	return 0;
}

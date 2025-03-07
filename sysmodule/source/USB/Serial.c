#include <string.h>
#include <stdatomic.h>
#include <stdio.h>

#include "Serial.h"
#include "UsbComms.h"

// We need the thread running flag
#include "../modes/modes.h"

static Mutex UsbStreamingMutex;

static const char* GetDeviceSerial() 
{
	static char serialStr[50] = "SysDVR:Unknown serial";
	static bool initialized = false;

	if (!initialized) {
		initialized = true;

		Result rc = setsysInitialize();
		if (R_SUCCEEDED(rc))
		{
			SetSysSerialNumber serial;
			rc = setsysGetSerialNumber(&serial);
			
			if (R_SUCCEEDED(rc))
				snprintf(serialStr, sizeof(serialStr), "SysDVR:%s", serial.number);
			
			setsysExit();
		}		
	}

	return serialStr;
}

Result UsbStreamingInitialize()
{
	mutexInit(&UsbStreamingMutex);

	UsbSerailInterfaceInfo interfaces = {
		.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
		.bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
		.bInterfaceProtocol = USB_CLASS_VENDOR_SPEC
	};

	UsbSerialInitializationInfo info = {
		// Google Nexus (generic) so we can use Google's signed WinUSB drivers instead of needing zadig
		.VendorId = 0x18D1,
		.ProductId = 0x4EE0,

		.DeviceName = "SysDVR",
		.DeviceManufacturer = "https://github.com/exelix11/SysDVR",
		.DeviceSerialNumber = GetDeviceSerial(),

		.NumInterfaces = 1,
		.Interfaces = { interfaces }
	};

	return usbSerialInitialize(&info);
}

void UsbStreamingExit()
{
	usbSerialExit();
}

UsbStreamRequest UsbStreamingWaitConnection()
{
	// Since USB is single-threaded now, in theory nothing else should be going on over usb at this point
	mutexLock(&UsbStreamingMutex);

	u32 request = 0;
	size_t read = 0;

	do
		read = usbSerialRead(&request, sizeof(request), 1E+9);
	while (read == 0 && IsThreadRunning);

	mutexUnlock(&UsbStreamingMutex);

	if (read != sizeof(request) || !IsThreadRunning)
		return UsbStreamRequestFailed;

	LOG("USB request received: %x\n", request);
	if (request == UsbStreamRequestVideo || request == UsbStreamRequestAudio || request == UsbStreamRequestBoth)
		return (UsbStreamRequest)request;

	return UsbStreamRequestFailed;
}

bool UsbStreamingSend(const void* data, size_t length)
{
	mutexLock(&UsbStreamingMutex);

	size_t sent = usbSerialWrite(data, length, 1E+9);

	mutexUnlock(&UsbStreamingMutex);

	return sent == length;
}
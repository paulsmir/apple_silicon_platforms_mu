/** @file
 *MsPlatformDevicesLib  - Device specific library.

Copyright (C) Microsoft Corporation. All rights reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Protocol/DevicePath.h>

#include <Library/AcpiPlatformUpdateLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MsPlatformDevicesLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Configuration/BootDevices.h>

//
// Serial console device path.
//
// This has to match, byte for byte, the path that EmbeddedPkg's SimpleTextInOutSerial
// installs (see SimpleTextInOut.c: SIMPLE_TEXT_OUT_DEVICE_PATH mDevicePath), otherwise
// BDS will not connect it: a vendor node carrying that driver's FILE_GUID, followed by a
// UART node built from the same PCDs.
//
// Needed because the declarations below only ever listed the internal panel and keypad.
// On a machine whose panel is dead (and where "DeviceBootManagerAfterConsole Unabled to
// set graphics - Unsupported" is what the log says) that leaves no console at all, so the
// UEFI Shell has nowhere to read from and exits immediately -- after which BDS runs out
// of boot options and calls PSCI SYSTEM_RESET.
//
#define SERIAL_CONSOLE_DRIVER_GUID                                                     \
  { 0x6696936D, 0x3637, 0x467C, { 0x87, 0xCB, 0x14, 0xEA, 0x82, 0x48, 0x94, 0x8C } }

typedef struct {
  VENDOR_DEVICE_PATH        Guid;
  UART_DEVICE_PATH          Uart;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SERIAL_CONSOLE_DEVICE_PATH;

STATIC SERIAL_CONSOLE_DEVICE_PATH SerialConsoleDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    SERIAL_CONSOLE_DRIVER_GUID
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP, { sizeof (UART_DEVICE_PATH), 0 } },
    0,                                      // Reserved
    FixedPcdGet64 (PcdUartDefaultBaudRate),
    FixedPcdGet8 (PcdUartDefaultDataBits),
    FixedPcdGet8 (PcdUartDefaultParity),
    FixedPcdGet8 (PcdUartDefaultStopBits)
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// Predefined platform default console device path
//
BDS_CONSOLE_CONNECT_ENTRY gPlatformConsoles[] =
{
  {
    (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath,
    CONSOLE_IN
  },
  {
    (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath,
    CONSOLE_OUT | STD_ERROR
  },
  {
    (EFI_DEVICE_PATH_PROTOCOL *)&SerialConsoleDevicePath,
    CONSOLE_IN | CONSOLE_OUT | STD_ERROR
  },
  {
    NULL,
    0
  }
};

EFI_DEVICE_PATH_PROTOCOL *gPlatformConInDeviceList[] = {
  (EFI_DEVICE_PATH_PROTOCOL *)&SerialConsoleDevicePath,
  NULL
};

/**
Library function used to provide the platform SD Card device path
**/
EFI_DEVICE_PATH_PROTOCOL *EFIAPI GetSdCardDevicePath(VOID)
{
  return (EFI_DEVICE_PATH_PROTOCOL *)&SdcardDevicePath;
}

/**
  Library function used to determine if the DevicePath is a valid bootable 'USB'
device. USB here indicates the port connection type not the device protocol.
  With TBT or USB4 support PCIe storage devices are valid 'USB' boot options.
**/
BOOLEAN
EFIAPI
PlatformIsDevicePathUsb(IN EFI_DEVICE_PATH_PROTOCOL *DevicePath)
{
  EFI_DEVICE_PATH_PROTOCOL *Node;

  for (Node = DevicePath; !IsDevicePathEnd(Node);
       Node = NextDevicePathNode(Node)) {
    if ((DevicePathType(Node) == MESSAGING_DEVICE_PATH) &&
        ((DevicePathSubType(Node) == MSG_USB_CLASS_DP) ||
         (DevicePathSubType(Node) == MSG_USB_WWID_DP) ||
         (DevicePathSubType(Node) == MSG_USB_DP))) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
Library function used to provide the list of platform devices that MUST be
connected at the beginning of BDS
**/
EFI_DEVICE_PATH_PROTOCOL **EFIAPI GetPlatformConnectList(VOID)
{
  PlatformUpdateAcpiTables();
  return NULL;
}

/**
 * Library function used to provide the list of platform console devices.
 */
BDS_CONSOLE_CONNECT_ENTRY *EFIAPI GetPlatformConsoleList(VOID)
{
  return (BDS_CONSOLE_CONNECT_ENTRY *)&gPlatformConsoles;
}

/**
Library function used to provide the list of platform devices that MUST be
connected to support ConsoleIn activity.  This call occurs on the ConIn connect
event, and allows platforms to do specific enablement for ConsoleIn support.
**/
EFI_DEVICE_PATH_PROTOCOL **EFIAPI GetPlatformConnectOnConInList(VOID)
{
  return (EFI_DEVICE_PATH_PROTOCOL **)&gPlatformConInDeviceList;
}

/**
Library function used to provide the console type.  For ConType == DisplayPath,
device path is filled in to the exact controller to use.  For other ConTypes,
DisplayPath must NULL. The device path must NOT be freed.
**/
EFI_HANDLE
EFIAPI
GetPlatformPreferredConsole(OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath)
{
  EFI_STATUS                Status;
  EFI_HANDLE                Handle = NULL;
  EFI_DEVICE_PATH_PROTOCOL *TempDevicePath;

  TempDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath;

  Status = gBS->LocateDevicePath(
      &gEfiGraphicsOutputProtocolGuid, &TempDevicePath, &Handle);
  if (!EFI_ERROR(Status) && IsDevicePathEnd(TempDevicePath)) {
  }
  else {
    DEBUG(
        (DEBUG_ERROR,
         "%a - Unable to locate platform preferred console. Code=%r\n",
         __FUNCTION__, Status));
    Status = EFI_DEVICE_ERROR;
  }

  if (Handle != NULL) {
    //
    // Connect the GOP driver
    //
    gBS->ConnectController(Handle, NULL, NULL, TRUE);

    //
    // Get the GOP device path
    // NOTE: We may get a device path that contains Controller node in it.
    //
    TempDevicePath = EfiBootManagerGetGopDevicePath(Handle);
    *DevicePath    = TempDevicePath;
  }

  return Handle;
}
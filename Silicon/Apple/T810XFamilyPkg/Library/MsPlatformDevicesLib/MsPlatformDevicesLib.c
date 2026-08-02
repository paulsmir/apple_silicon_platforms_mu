/** @file
 *MsPlatformDevicesLib  - Device specific library.

Copyright (C) Microsoft Corporation. All rights reserved.
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Protocol/DevicePath.h>

#include <Guid/SerialPortLibVendor.h>
#include <Guid/TtyTerm.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsPlatformDevicesLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>

typedef struct {
  VENDOR_DEVICE_PATH       DisplayDevicePath;
  EFI_DEVICE_PATH_PROTOCOL EndDevicePath;
} EFI_DISPLAY_DEVICE_PATH;

EFI_DISPLAY_DEVICE_PATH DisplayDevicePath =
{
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(END_DEVICE_PATH_LENGTH),
      (UINT8)((END_DEVICE_PATH_LENGTH) >> 8)
    }
  }
};
//
// Serial console device path.
//
// The serial console: SerialDxe's SerialIo handle, with a terminal-type node appended.
//
// Before this the platform declared only DisplayDevicePath (CONSOLE_OUT) and an empty
// gPlatformConInDeviceList -- i.e. no console input at all. On this machine the panel is
// dead and graphics come up "Unsupported", so the UEFI Shell had nothing to read from,
// exited immediately, and BDS then ran out of options and called PSCI SYSTEM_RESET.
//
// The first fix pointed this path at EmbeddedPkg's SimpleTextInOutSerial instead, which
// works for a shell but provides only SimpleTextIn. The Windows boot manager opens
// SimpleTextInputEx and gives up when it is not there:
//
//   BS: OpenProtocol handle=... DD9E7534-7762-4698-8C14-F58517A625AA attr=0x2 -> Unsupported
//   ... StartImage -> Invalid Parameter
//
// TerminalDxe produces both, but it binds to a SerialIo handle whose device path ends in a
// terminal-type vendor node, and SerialDxe's own path is only VenHw(SerialPortLibVendor) +
// UART. Appending the terminal node here is what lets it bind; the child handle it then
// creates is the console that carries SimpleTextInputEx.
//
typedef struct {
  VENDOR_DEVICE_PATH        Guid;
  UART_DEVICE_PATH          Uart;
  VENDOR_DEVICE_PATH        Terminal;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SERIAL_CONSOLE_DEVICE_PATH;

STATIC SERIAL_CONSOLE_DEVICE_PATH SerialConsoleDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    EDKII_SERIAL_PORT_LIB_VENDOR_GUID
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP, { sizeof (UART_DEVICE_PATH), 0 } },
    0,                                      // Reserved
    FixedPcdGet64 (PcdUartDefaultBaudRate),
    FixedPcdGet8 (PcdUartDefaultDataBits),
    FixedPcdGet8 (PcdUartDefaultParity),
    FixedPcdGet8 (PcdUartDefaultStopBits)
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    EFI_TTY_TERM_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// Predefined platform default console device path
//
BDS_CONSOLE_CONNECT_ENTRY gPlatformConsoles[] =
{
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
EFI_DEVICE_PATH_PROTOCOL *
EFIAPI
GetSdCardDevicePath (
  VOID
  )
{
  return NULL;
}

/**
  Library function used to determine if the DevicePath is a valid bootable 'USB' device.
  USB here indicates the port connection type not the device protocol.
  With TBT or USB4 support PCIe storage devices are valid 'USB' boot options.
**/
BOOLEAN
EFIAPI
PlatformIsDevicePathUsb (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  return FALSE;
}

/**
Library function used to provide the list of platform devices that MUST be
connected at the beginning of BDS
**/
EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
GetPlatformConnectList (
  VOID
  )
{
  return NULL;
}

/**
  Point gST->ConOut at the serial console. See the ReadyToBoot hook below for why this is
  done here and not during console setup.
**/
STATIC
VOID
EFIAPI
SerialConsoleOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       Handle   = NULL;
  EFI_DEVICE_PATH_PROTOCOL         *WalkPath =
    (EFI_DEVICE_PATH_PROTOCOL *)&SerialConsoleDevicePath;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *TextOut = NULL;

  Status = gBS->LocateDevicePath (&gEfiSimpleTextOutProtocolGuid, &WalkPath, &Handle);
  if (EFI_ERROR (Status) || (Handle == NULL)) {
    DEBUG ((DEBUG_ERROR, "SERIALCON: ReadyToBoot - handle not found: %r\n", Status));
    return;
  }

  Status = gBS->HandleProtocol (Handle, &gEfiSimpleTextOutProtocolGuid, (VOID **)&TextOut);
  if (EFI_ERROR (Status) || (TextOut == NULL)) {
    DEBUG ((DEBUG_ERROR, "SERIALCON: ReadyToBoot - no SimpleTextOut: %r\n", Status));
    return;
  }

  gST->ConsoleOutHandle = Handle;
  gST->ConOut           = TextOut;
  gST->StandardErrorHandle = Handle;
  gST->StdErr              = TextOut;

  //
  // The system table is CRC-checked, so it has to be recomputed after editing it.
  //
  gST->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gST, gST->Hdr.HeaderSize, &gST->Hdr.CRC32);

  DEBUG ((DEBUG_ERROR, "SERIALCON: ConOut repointed to the serial console\n"));
}

/**
 * Library function used to provide the list of platform console devices.
 */
BDS_CONSOLE_CONNECT_ENTRY *
EFIAPI
GetPlatformConsoleList (
  VOID
  )
{
  //
  // BRING-UP DIAGNOSTIC: dump the serial console path we declare, to compare against the
  // one SimpleTextInOutSerial actually installs (it prints its own). ConPlatformDxe
  // matches device paths exactly, and OutputString is never called, so a mismatch here
  // would explain why this console never lands in ConOut.
  //
  {
    UINT8  *Bytes = (UINT8 *)&SerialConsoleDevicePath;
    UINTN   Idx;

    DEBUG ((DEBUG_ERROR, "SERIALCON: platform path (%d bytes):",
            (int)sizeof (SerialConsoleDevicePath)));
    for (Idx = 0; Idx < sizeof (SerialConsoleDevicePath); Idx++) {
      DEBUG ((DEBUG_ERROR, " %02x", Bytes[Idx]));
    }
    DEBUG ((DEBUG_ERROR, "\n"));
  }

  //
  // Repoint gST->ConOut at the serial console when ReadyToBoot fires.
  //
  // Everything up to here is already correct: the device path matches the driver's byte
  // for byte, and the ConOut variable contains it (dumped: 67 bytes, GOP + serial).
  // ConSplitter still does not route to it -- SimpleTextInOutSerial installs its protocols
  // on a handle it makes itself and assigns gST->ConIn/ConOut directly, and ConSplitter
  // later overwrites gST->ConOut with an aggregate that does not include it. Input kept
  // working precisely because the driver's gST->ConIn assignment survived.
  //
  // Doing the switch at ReadyToBoot rather than earlier matters: when the serial console
  // was made the *preferred* console, MsBootPolicy hung ("Unable to set console mode -
  // Unsupported") long before the Shell was reached. ReadyToBoot is signalled immediately
  // before StartImage, so all the graphics-dependent BDS work is already done.
  //
  {
    STATIC EFI_EVENT  ReadyToBootEvent = NULL;

    if (ReadyToBootEvent == NULL) {
      EFI_STATUS  RtbStatus;

      RtbStatus = EfiCreateEventReadyToBootEx (
                    TPL_CALLBACK,
                    SerialConsoleOnReadyToBoot,
                    NULL,
                    &ReadyToBootEvent
                    );
      DEBUG ((DEBUG_ERROR, "SERIALCON: ReadyToBoot hook = %r\n", RtbStatus));
    }
  }

  //
  // Connect the serial console handle explicitly.
  //
  // EmbeddedPkg's SimpleTextInOutSerial installs SimpleTextIn/Out plus a device path on a
  // handle it creates itself, and then assigns gST->ConIn/ConOut directly. Nothing ever
  // calls ConnectController() on that handle, so ConPlatformDxe never binds to it, it
  // never gets gEfiConsoleOutDeviceGuid, and ConSplitter therefore leaves it out of the
  // aggregate ConOut it later installs into gST -- which is why the Shell's output never
  // reached the UART even though its device path *is* present in the ConOut variable
  // (verified by dumping it: 67 bytes, GOP instance + this one).
  //
  // Input kept working because the driver's own gST->ConIn assignment survived.
  //
  // This runs at BDS time, so every driver is already loaded.
  //
  {
    EFI_STATUS                Status;
    EFI_HANDLE                Handle     = NULL;
    EFI_DEVICE_PATH_PROTOCOL  *WalkPath  =
      (EFI_DEVICE_PATH_PROTOCOL *)&SerialConsoleDevicePath;

    Status = gBS->LocateDevicePath (&gEfiSimpleTextOutProtocolGuid, &WalkPath, &Handle);
    if (!EFI_ERROR (Status) && (Handle != NULL)) {
      Status = gBS->ConnectController (Handle, NULL, NULL, FALSE);
      DEBUG ((DEBUG_ERROR, "SERIALCON: ConnectController on serial console = %r\n", Status));
    } else {
      DEBUG ((DEBUG_ERROR, "SERIALCON: serial console handle not found = %r\n", Status));
    }
  }

  return (BDS_CONSOLE_CONNECT_ENTRY *)&gPlatformConsoles;
}

/**
Library function used to provide the list of platform devices that MUST be connected
to support ConsoleIn activity.  This call occurs on the ConIn connect event, and
allows platforms to do enable specific devices ConsoleIn support.
**/
EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
GetPlatformConnectOnConInList (
  VOID
  )
{
  return NULL;
}

/**
Library function used to provide the console type.  For ConType == DisplayPath,
device path is filled in to the exact controller to use.  For other ConTypes, DisplayPath
must NULL. The device path must NOT be freed.
**/
EFI_HANDLE
EFIAPI
GetPlatformPreferredConsole (
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath
  )
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

  //
  // NOTE: do NOT return the serial console from here. It was tried: BDS then uses it as
  // ConOut, and MsBootPolicy hangs at "USB boot desired, but no USB devices found on first
  // attempt" and never reaches the Shell -- the same failure as dropping GraphicsConsoleDxe
  // altogether. It apparently needs a console that supports mode setting
  // ("MsBootPolicyEntry Unable to set console mode - Unsupported").
  //
  // Shell input over serial already works (ConIn); only its output still goes to the dead
  // internal panel. That has to be solved without changing the preferred console.
  //
  return Handle;
}

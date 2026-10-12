/** @file
  Boot medium preloaded into guest RAM by the launcher (run_uefi.py).
  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
**/
#ifndef APPLE_PRELOADED_RAMDISK_H_
#define APPLE_PRELOADED_RAMDISK_H_

#include <Base.h>

//
// run_uefi.py / BootRamdiskHelperDxe ABI (values unchanged; ans-native carries the
// same header). The payload is placed one page in so the disk image stays page
// aligned.
//
#define PRELOADED_RAMDISK_MAGIC           "ASIRAMDK"
#define PRELOADED_RAMDISK_PAYLOAD_OFFSET  0x1000

#pragma pack(1)
typedef struct {
  CHAR8   Magic[8];
  UINT64  Size;
} PRELOADED_RAMDISK_HEADER;
#pragma pack()

//
// M1: the window holds a boot medium only when the launcher wrote this header;
// without one the window is ordinary system memory. MemoryInitPeiLib decides once
// (it reads the header with the MMU off, so no BaseMemoryLib routine that may use
// unaligned loads) and publishes the decision in a GUID HOB; BootRamdiskHelperDxe
// registers a RAM disk only from that HOB, so DXE can never register pages that
// PEI handed to the OS.
//
#define PRELOADED_RAMDISK_DECISION_VERSION  1

typedef struct {
  UINT32  Version;
  UINT32  Present;
  UINT64  PayloadBase;
  UINT64  PayloadSize;
} PRELOADED_RAMDISK_DECISION;

/**
  TRUE when Header (at WindowBase, a window of WindowSize bytes) carries the magic
  and a payload size that is non-zero and fits after the header page.
**/
STATIC inline
BOOLEAN
PreloadedRamdiskHeaderValid (
  IN CONST PRELOADED_RAMDISK_HEADER  *Header,
  IN UINT64                          WindowBase,
  IN UINT64                          WindowSize
  )
{
  CONST CHAR8  *Magic;
  UINTN        Index;

  Magic = PRELOADED_RAMDISK_MAGIC;
  if ((Header == NULL) || (WindowBase == 0) ||
      (WindowSize <= PRELOADED_RAMDISK_PAYLOAD_OFFSET) ||
      (WindowBase > MAX_UINT64 - WindowSize))
  {
    return FALSE;
  }

  for (Index = 0; Index < sizeof (Header->Magic); Index++) {
    if (Header->Magic[Index] != Magic[Index]) {
      return FALSE;
    }
  }

  return (BOOLEAN)((Header->Size != 0) &&
                   (Header->Size <= WindowSize - PRELOADED_RAMDISK_PAYLOAD_OFFSET));
}

#endif

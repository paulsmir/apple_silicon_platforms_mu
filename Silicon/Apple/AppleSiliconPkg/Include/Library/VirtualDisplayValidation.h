/** @file
  Pure validation helpers shared by virtual-display PEI and GOP code.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIRTUAL_DISPLAY_VALIDATION_H_
#define VIRTUAL_DISPLAY_VALIDATION_H_

#ifdef VIRTUAL_DISPLAY_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool BOOLEAN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void VOID;

#define TRUE  true
#define FALSE false
#define IN
#define OUT
#define VD_STATIC_INLINE static inline
#define EFI_SUCCESS           UINT64_C(0)
#define EFI_INVALID_PARAMETER UINT64_C(0x8000000000000002)
#define EFI_UNSUPPORTED       UINT64_C(0x8000000000000003)
#define MAX_UINT32            UINT32_C(0xffffffff)
#define MAX_UINT64            UINT64_C(0xffffffffffffffff)
#else
#include <Uefi.h>
#include <Base.h>
#define VD_STATIC_INLINE STATIC inline
#endif

#define VIRTUAL_DISPLAY_PAGE_MASK  0xfffULL
#define VIRTUAL_DISPLAY_BPP        32U
#define VIRTUAL_DISPLAY_BYTE_DEPTH 4U

VD_STATIC_INLINE
EFI_STATUS
VirtualDisplayValidateQuery (
  IN VOID    *This,
  IN UINT32  ModeNumber,
  OUT UINTN  *SizeOfInfo,
  OUT VOID   **Info
  )
{
  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return (ModeNumber == 0) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

VD_STATIC_INLINE
EFI_STATUS
VirtualDisplayValidateSetMode (
  IN VOID    *This,
  IN UINT32  ModeNumber
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return (ModeNumber == 0) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

VD_STATIC_INLINE
BOOLEAN
VirtualDisplayValidateGeometry (
  IN UINT32  Width,
  IN UINT32  Height,
  IN UINT32  Stride,
  IN UINT32  Depth
  )
{
  if ((Width == 0) || (Height == 0) || (Depth != VIRTUAL_DISPLAY_BPP)) {
    return FALSE;
  }

  if ((Stride & (VIRTUAL_DISPLAY_BYTE_DEPTH - 1)) != 0) {
    return FALSE;
  }

  return Stride >= ((UINT64)Width * VIRTUAL_DISPLAY_BYTE_DEPTH);
}

VD_STATIC_INLINE
BOOLEAN
VirtualDisplayValidateFramebufferRange (
  IN UINT64  Base,
  IN UINT64  Size,
  IN UINT64  SystemMemoryBase,
  IN UINT64  SystemMemorySize
  )
{
  UINT64  End;
  UINT64  SystemMemoryEnd;

  if ((Base == 0) || (Size == 0) || (Size > MAX_UINT32) ||
      ((Base | Size) & VIRTUAL_DISPLAY_PAGE_MASK) != 0) {
    return FALSE;
  }

  if ((Base > (MAX_UINT64 - Size)) ||
      (SystemMemoryBase > (MAX_UINT64 - SystemMemorySize))) {
    return FALSE;
  }

  End             = Base + Size;
  SystemMemoryEnd = SystemMemoryBase + SystemMemorySize;
  return (Base >= SystemMemoryBase) && (End <= SystemMemoryEnd);
}

#endif

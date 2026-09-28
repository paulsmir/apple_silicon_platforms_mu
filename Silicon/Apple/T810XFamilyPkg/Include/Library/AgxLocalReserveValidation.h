/** @file
  J313 AGX local-memory PEI range validation.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef AGX_LOCAL_RESERVE_VALIDATION_H_
#define AGX_LOCAL_RESERVE_VALIDATION_H_

#include <Base.h>

#ifdef AGX_LOCAL_RESERVE_V2
#define AGX_LOCAL_RESERVE_BYTES 0x40000000ULL
#define AGX_LOCAL_RESERVE_ALIGNMENT 0x10000ULL
#define AGX_LOCAL_BROKER_VERSION 2U
#else
#define AGX_LOCAL_RESERVE_BYTES 0x04000000ULL
#define AGX_LOCAL_RESERVE_ALIGNMENT AGX_LOCAL_RESERVE_BYTES
#define AGX_LOCAL_BROKER_VERSION 1U
#endif
#define AGX_LOCAL_BROKER_BASE 0x300000D00ULL
#define AGX_LOCAL_BROKER_MAGIC 0x4C584741U

typedef struct {
  UINT64 Base;
  UINT64 Size;
} AGX_LOCAL_RANGE;

static inline BOOLEAN
AgxLocalReserveDisjoint (IN UINT64 Base, IN UINT64 Other, IN UINT64 Size)
{
  if (Size == 0 || Other > (~(UINT64)0) - Size ||
      Base > (~(UINT64)0) - AGX_LOCAL_RESERVE_BYTES) {
    return FALSE;
  }
  return Base >= Other + Size || Other >= Base + AGX_LOCAL_RESERVE_BYTES;
}

static inline BOOLEAN
AgxLocalReserveRangeValid (
  IN UINT64 Base,
  IN UINT64 Size,
  IN UINT64 RamBase,
  IN UINT64 RamSize,
  IN CONST AGX_LOCAL_RANGE *Excluded,
  IN UINTN ExcludedCount
  )
{
  UINT64 End;
  UINT64 RamEnd;
  UINTN Index;

  if (Size != AGX_LOCAL_RESERVE_BYTES ||
      (Base & (AGX_LOCAL_RESERVE_ALIGNMENT - 1ULL)) != 0 ||
      (ExcludedCount != 0 && Excluded == NULL) ||
      RamSize == 0 || Base > (~(UINT64)0) - Size ||
      RamBase > (~(UINT64)0) - RamSize) {
    return FALSE;
  }
  End = Base + Size;
  RamEnd = RamBase + RamSize;
  if (Base < RamBase || End > RamEnd) {
    return FALSE;
  }
  for (Index = 0; Index < ExcludedCount; ++Index) {
    UINT64 ExcludedEnd;
    if (Excluded[Index].Size == 0 ||
        Excluded[Index].Base > (~(UINT64)0) - Excluded[Index].Size) {
      return FALSE;
    }
    ExcludedEnd = Excluded[Index].Base + Excluded[Index].Size;
    if (Base < ExcludedEnd && Excluded[Index].Base < End) {
      return FALSE;
    }
  }
  return TRUE;
}

#endif

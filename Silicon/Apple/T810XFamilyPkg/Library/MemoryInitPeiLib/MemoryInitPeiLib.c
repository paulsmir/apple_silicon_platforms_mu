/**
 * @file MemoryInitPeiLib.c
 * 
 * @author amarioguy (Arminder Singh)
 * 
 * This file implements page table setup, memory HOB setup, and MMU initialization.
 * Adapted from SurfaceDuoPkg/MemoryInitPeiLib.c
 * 
 * @version 1.0
 * @date 2022-07-31
 * 
 * @copyright Copyright (c) amarioguy (Arminder Singh) 2022.
 * 
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * 
 **/

#include <PiPei.h>

#include <Library/ArmMmuLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/AppleDTLib.h>
#include <Library/VirtualDisplayValidation.h>
#include <Library/AgxLocalReserveValidation.h>
#include <Library/PreloadedRamdisk.h>

//Device memory map configuration file for UEFI (this is to help with pagetable initialization)
#include <Library/T810XFamilyVirtualMemoryMapDefines.h>

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS 20  // low-memory window and AGX broker

#define DDR_ATTRIBUTES_CACHED           ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define DDR_ATTRIBUTES_UNCACHED         ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED

VOID BuildMemoryTypeInformationHob(VOID);

VOID BuildVirtualMemoryMap(OUT ARM_MEMORY_REGION_DESCRIPTOR **VirtualMemoryMap);

STATIC VOID InitMmu(IN ARM_MEMORY_REGION_DESCRIPTOR *MemoryTable)
{
    VOID *MemoryTranslationTableBase;
    UINTN MemoryTranslationTableSize;
    RETURN_STATUS StatusCode;

    DEBUG(
        (DEBUG_INFO, 
        "MemoryInitPeiLib: Enabling MMU, Page Table Base: 0x%p, Page Table Size: 0x%p\n", 
        &MemoryTranslationTableBase, &MemoryTranslationTableSize)
        );
    StatusCode = ArmConfigureMmu(MemoryTable, &MemoryTranslationTableBase, &MemoryTranslationTableSize);

    if(EFI_ERROR(StatusCode))
    {
        DEBUG((DEBUG_ERROR | DEBUG_INFO, "MemoryInitPeiLib: MMU enable failed!! Status: %llx\n", StatusCode));
    }
}

//borrowed from edk2-platforms:Armada7k8kMemoryInitPeiLib
STATIC VOID ReserveMemoryRegion ( IN EFI_PHYSICAL_ADDRESS ReservedRegionBase, IN UINT32 ReservedRegionSize)
{
  EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttributes;
  EFI_PHYSICAL_ADDRESS         ReservedRegionTop;
  EFI_PHYSICAL_ADDRESS         ResourceTop;
  EFI_PEI_HOB_POINTERS         NextHob;
  UINT64                       ResourceLength;

  ReservedRegionTop = ReservedRegionBase + ReservedRegionSize;

  //
  // Search for System Memory Hob that covers the reserved region,
  // and punch a hole in it
  //
  for (NextHob.Raw = GetHobList ();
       NextHob.Raw != NULL;
       NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR,
                                 NextHob.Raw)) {

    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (ReservedRegionBase >= NextHob.ResourceDescriptor->PhysicalStart) &&
        (ReservedRegionTop <= NextHob.ResourceDescriptor->PhysicalStart +
                      NextHob.ResourceDescriptor->ResourceLength))
    {
      ResourceAttributes = NextHob.ResourceDescriptor->ResourceAttribute;
      ResourceLength = NextHob.ResourceDescriptor->ResourceLength;
      ResourceTop = NextHob.ResourceDescriptor->PhysicalStart + ResourceLength;

      if (ReservedRegionBase == NextHob.ResourceDescriptor->PhysicalStart) {
        //
        // This region starts right at the start of the reserved region, so we
        // can simply move its start pointer and reduce its length by the same
        // value
        //
        NextHob.ResourceDescriptor->PhysicalStart += ReservedRegionSize;
        NextHob.ResourceDescriptor->ResourceLength -= ReservedRegionSize;

      } else if ((NextHob.ResourceDescriptor->PhysicalStart +
                  NextHob.ResourceDescriptor->ResourceLength) ==
                  ReservedRegionTop) {

        //
        // This region ends right at the end of the reserved region, so we
        // can simply reduce its length by the size of the region.
        //
        NextHob.ResourceDescriptor->ResourceLength -= ReservedRegionSize;

      } else {
        //
        // This region covers the reserved region. So split it into two regions,
        // each one touching the reserved region at either end, but not covering
        // it.
        //
        NextHob.ResourceDescriptor->ResourceLength =
                 ReservedRegionBase - NextHob.ResourceDescriptor->PhysicalStart;

        // Create the System Memory HOB for the remaining region (top of the FD)
        BuildResourceDescriptorHob (EFI_RESOURCE_SYSTEM_MEMORY,
                                    ResourceAttributes,
                                    ReservedRegionTop,
                                    ResourceTop - ReservedRegionTop);
      }

      //
      // Reserve the memory space.
      //
      BuildResourceDescriptorHob (EFI_RESOURCE_MEMORY_RESERVED,
        0,
        ReservedRegionBase,
        ReservedRegionSize);

      break;
    }
    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }
}

STATIC BOOLEAN AgxLocalReserveHobContains (IN UINT64 Base)
{
  EFI_PEI_HOB_POINTERS Hob;
  UINT64 End = Base + AGX_LOCAL_RESERVE_BYTES;

  Hob.Raw = GetHobList ();
  while ((Hob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, Hob.Raw)) != NULL) {
    if (Hob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY &&
        Base >= Hob.ResourceDescriptor->PhysicalStart &&
        Hob.ResourceDescriptor->ResourceLength >= AGX_LOCAL_RESERVE_BYTES &&
        End <= Hob.ResourceDescriptor->PhysicalStart +
               Hob.ResourceDescriptor->ResourceLength) {
      return TRUE;
    }
    Hob.Raw = GET_NEXT_HOB (Hob);
  }
  return FALSE;
}


STATIC BOOLEAN AgxLocalReserveHobsSafe (IN UINT64 Base)
{
  EFI_PEI_HOB_POINTERS Hob;
  Hob.Raw = GetHobList ();
  while (!END_OF_HOB_LIST (Hob)) {
    UINT64 Other = 0;
    UINT64 Size = 0;
    switch (Hob.Header->HobType) {
    case EFI_HOB_TYPE_HANDOFF:
      Other = Hob.HandoffInformationTable->EfiMemoryBottom;
      if (Hob.HandoffInformationTable->EfiMemoryTop <= Other) return FALSE;
      Size = Hob.HandoffInformationTable->EfiMemoryTop - Other;
      break;
    case EFI_HOB_TYPE_MEMORY_ALLOCATION:
      Other = Hob.MemoryAllocation->AllocDescriptor.MemoryBaseAddress;
      Size = Hob.MemoryAllocation->AllocDescriptor.MemoryLength;
      break;
    case EFI_HOB_TYPE_FV:
    case EFI_HOB_TYPE_FV2:
    case EFI_HOB_TYPE_FV3:
      // FV/FV2/FV3 share Header, BaseAddress and Length as their prefix.
      Other = Hob.FirmwareVolume->BaseAddress;
      Size = Hob.FirmwareVolume->Length;
      break;
    default:
      Hob.Raw = GET_NEXT_HOB (Hob);
      continue;
    }
    if (!AgxLocalReserveDisjoint (Base, Other, Size)) return FALSE;
    Hob.Raw = GET_NEXT_HOB (Hob);
  }
  return TRUE;
}

//Borrowed from ArmPlatformPkg
EFI_STATUS EFIAPI MemoryPeim(IN EFI_PHYSICAL_ADDRESS UefiMemoryBase, IN UINT64 UefiMemorySize)
{
  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable;
  EFI_RESOURCE_ATTRIBUTE_TYPE   ResourceAttributes;
  UINT64                        ResourceLength;
  EFI_PEI_HOB_POINTERS          NextHob;
  EFI_PHYSICAL_ADDRESS          FdTop;
  EFI_PHYSICAL_ADDRESS          SystemMemoryTop;
  EFI_PHYSICAL_ADDRESS          ResourceTop;
  EFI_PHYSICAL_ADDRESS          FrameBufferBase;
  EFI_PHYSICAL_ADDRESS          LocalBase;
  UINT64                        LocalHostBase;
  UINT64                        LocalBytes;
  AGX_LOCAL_RANGE               LocalExcluded[5];
  UINTN                         LocalExcludedCount;
  UINT64                        FrameBufferSize;
  BOOLEAN                       Found;
  CONST PRELOADED_RAMDISK_HEADER  *RamdiskHeader;
  PRELOADED_RAMDISK_DECISION    RamdiskDecision;

  // build up virtual memory map
  BuildVirtualMemoryMap(&MemoryTable);

  // Ensure PcdSystemMemorySize has been set
  ASSERT (PcdGet64 (PcdSystemMemorySize) != 0);

  //
  // Now, the permanent memory has been installed, we can call AllocatePages()
  //
  ResourceAttributes = (
                        EFI_RESOURCE_ATTRIBUTE_PRESENT |
                        EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
                        EFI_RESOURCE_ATTRIBUTE_TESTED
                        );

  //
  // Check if the resource for the main system memory has been declared
  //
  Found       = FALSE;
  NextHob.Raw = GetHobList ();
  while ((NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw)) != NULL) {
    if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
        (PcdGet64 (PcdSystemMemoryBase) >= NextHob.ResourceDescriptor->PhysicalStart) &&
        (NextHob.ResourceDescriptor->PhysicalStart + NextHob.ResourceDescriptor->ResourceLength <= PcdGet64 (PcdSystemMemoryBase) + PcdGet64 (PcdSystemMemorySize)))
    {
      Found = TRUE;
      break;
    }

    NextHob.Raw = GET_NEXT_HOB (NextHob);
  }

  if (!Found) {
    // Reserved the memory space occupied by the firmware volume
    BuildResourceDescriptorHob (
      EFI_RESOURCE_SYSTEM_MEMORY,
      ResourceAttributes,
      PcdGet64 (PcdSystemMemoryBase),
      PcdGet64 (PcdSystemMemorySize)
      );
  }

  //
  // Reserved the memory space occupied by the firmware volume
  //

  SystemMemoryTop = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemoryBase) + (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdSystemMemorySize);
  FdTop           = (EFI_PHYSICAL_ADDRESS)PcdGet64 (PcdFdBaseAddress) + (EFI_PHYSICAL_ADDRESS)PcdGet32 (PcdFdSize);

  // EDK2 does not have the concept of boot firmware copied into DRAM. To avoid the DXE
  // core to overwrite this area we must create a memory allocation HOB for the region,
  // but this only works if we split off the underlying resource descriptor as well.
  if ((PcdGet64 (PcdFdBaseAddress) >= PcdGet64 (PcdSystemMemoryBase)) && (FdTop <= SystemMemoryTop)) {
    Found = FALSE;

    // Search for System Memory Hob that contains the firmware
    NextHob.Raw = GetHobList ();
    while ((NextHob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, NextHob.Raw)) != NULL) {
      if ((NextHob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
          (PcdGet64 (PcdFdBaseAddress) >= NextHob.ResourceDescriptor->PhysicalStart) &&
          (FdTop <= NextHob.ResourceDescriptor->PhysicalStart + NextHob.ResourceDescriptor->ResourceLength))
      {
        ResourceAttributes = NextHob.ResourceDescriptor->ResourceAttribute;
        ResourceLength     = NextHob.ResourceDescriptor->ResourceLength;
        ResourceTop        = NextHob.ResourceDescriptor->PhysicalStart + ResourceLength;

        if (PcdGet64 (PcdFdBaseAddress) == NextHob.ResourceDescriptor->PhysicalStart) {
          if (SystemMemoryTop != FdTop) {
            // Create the System Memory HOB for the firmware
            BuildResourceDescriptorHob (
              EFI_RESOURCE_SYSTEM_MEMORY,
              ResourceAttributes,
              PcdGet64 (PcdFdBaseAddress),
              PcdGet32 (PcdFdSize)
              );

            // Top of the FD is system memory available for UEFI
            NextHob.ResourceDescriptor->PhysicalStart  += PcdGet32 (PcdFdSize);
            NextHob.ResourceDescriptor->ResourceLength -= PcdGet32 (PcdFdSize);
          }
        } else {
          // Create the System Memory HOB for the firmware
          BuildResourceDescriptorHob (
            EFI_RESOURCE_SYSTEM_MEMORY,
            ResourceAttributes,
            PcdGet64 (PcdFdBaseAddress),
            PcdGet32 (PcdFdSize)
            );

          // Update the HOB
          NextHob.ResourceDescriptor->ResourceLength = PcdGet64 (PcdFdBaseAddress) - NextHob.ResourceDescriptor->PhysicalStart;

          // If there is some memory available on the top of the FD then create a HOB
          if (FdTop < NextHob.ResourceDescriptor->PhysicalStart + ResourceLength) {
            // Create the System Memory HOB for the remaining region (top of the FD)
            BuildResourceDescriptorHob (
              EFI_RESOURCE_SYSTEM_MEMORY,
              ResourceAttributes,
              FdTop,
              ResourceTop - FdTop
              );
          }
        }

        // Mark the memory covering the Firmware Device as boot services data
        BuildMemoryAllocationHob (
          PcdGet64 (PcdFdBaseAddress),
          PcdGet32 (PcdFdSize),
          EfiBootServicesData
          );

        Found = TRUE;
        break;
      }

      NextHob.Raw = GET_NEXT_HOB (NextHob);
    }

    ASSERT (Found);
  }

  //
  // The framebuffer is ordinary DRAM shared by GOP, Windows BasicDisplay, and the host
  // stream reader. Remove it from the system-memory resource and describe it as reserved
  // before DXE can allocate any of these pages for another purpose.
  //
  FrameBufferBase = PcdGet64 (PcdFrameBufferAddress);
  FrameBufferSize = PcdGet64 (PcdFrameBufferSize);
  if (!VirtualDisplayValidateFramebufferRange (
         FrameBufferBase,
         FrameBufferSize,
         PcdGet64 (PcdSystemMemoryBase),
         PcdGet64 (PcdSystemMemorySize)
         )) {
    DEBUG ((DEBUG_ERROR,
      "MemoryInitPeiLib: invalid framebuffer range 0x%llx + 0x%llx (RAM 0x%llx + 0x%llx)\n",
      FrameBufferBase, FrameBufferSize, PcdGet64 (PcdSystemMemoryBase),
      PcdGet64 (PcdSystemMemorySize)));
    ASSERT (FALSE);
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: reserving framebuffer 0x%llx + 0x%llx\n",
          FrameBufferBase, FrameBufferSize));
  ReserveMemoryRegion (FrameBufferBase, (UINT32)FrameBufferSize);
  BuildMemoryAllocationHob (FrameBufferBase, FrameBufferSize, EfiReservedMemoryType);
  
  //reserve secondary stacks carveouts passed into cpm-impl-reg 
  for(int i = 0; i < PcdGet32(PcdCoreCount); i++){
    CHAR8 CpuNodeName[14];
    UINTN CarveoutLength = 0;

    AsciiSPrint(CpuNodeName, ARRAY_SIZE(CpuNodeName), "/cpus/cpu%d", i);
    dt_node_t *CpuNode = dt_get(CpuNodeName);

    //
    // A CPU can legitimately be missing from the ADT: m1n1's hypervisor deletes the node of
    // whichever core it reserves for its own watchdog. The node has to be checked here and
    // not after reading the property - dt_node_prop() dereferences the node itself, so a
    // NULL node faults inside it, before there is any result to test.
    //
    if (CpuNode == NULL) {
      DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: %a is absent, skipping its carveout\n", CpuNodeName));
      continue;
    }

    UINT32 *Carveout = (UINT32 *)dt_node_prop(CpuNode, "cpm-impl-reg", &CarveoutLength);

    if ((Carveout == NULL) || (CarveoutLength < 4 * sizeof (UINT32))) {
      DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: no cpm-impl-reg for %a, skipping\n", CpuNodeName));
      continue;
    }

    ReserveMemoryRegion (
      ((UINT64)Carveout[1] << 32) | Carveout[0],
      ((UINT64)Carveout[3] << 32) | Carveout[2]
    );
  }



  //
  // Reserve the window a preloaded RAMDisk is dropped into. Anything larger than a few
  // tens of megabytes cannot travel inside the firmware volume - FvMain is decompressed
  // whole during PrePi, and a 64 MB image already fails there with Out of Resources - so
  // the image is written straight into guest memory instead and only registered here.
  // Punching the hole in PEI is what keeps DXE from allocating over it.
  //
  // M1: only a launcher that preloaded an image writes its header (run_uefi.py clears
  // the header page otherwise, and publishes the window to m1n1 as a ramdisk region so
  // GPU DMA stays out of it). Without a valid header the window stays system memory.
  // DXE registers a RAM disk only from the decision HOB built here.
  //
  if ((PcdGet64 (PcdPreloadedRamdiskBase) != 0) && (PcdGet32 (PcdPreloadedRamdiskMaxSize) != 0)) {
    RamdiskDecision.Version = PRELOADED_RAMDISK_DECISION_VERSION;
    RamdiskDecision.Present = 0;
    RamdiskDecision.PayloadBase = 0;
    RamdiskDecision.PayloadSize = 0;
    RamdiskHeader = (CONST PRELOADED_RAMDISK_HEADER *)(UINTN)PcdGet64 (PcdPreloadedRamdiskBase);
    if (PreloadedRamdiskHeaderValid (RamdiskHeader, PcdGet64 (PcdPreloadedRamdiskBase),
                                     PcdGet32 (PcdPreloadedRamdiskMaxSize)))
    {
      DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: reserving preloaded RAMDisk window 0x%llx + 0x%x\n",
              PcdGet64 (PcdPreloadedRamdiskBase), PcdGet32 (PcdPreloadedRamdiskMaxSize)));
      ReserveMemoryRegion (
        PcdGet64 (PcdPreloadedRamdiskBase),
        PcdGet32 (PcdPreloadedRamdiskMaxSize)
        );
      RamdiskDecision.Present = 1;
      RamdiskDecision.PayloadBase = PcdGet64 (PcdPreloadedRamdiskBase) + PRELOADED_RAMDISK_PAYLOAD_OFFSET;
      RamdiskDecision.PayloadSize = RamdiskHeader->Size;
    } else {
      DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: no preloaded RAMDisk header at 0x%llx; window is system memory\n",
              PcdGet64 (PcdPreloadedRamdiskBase)));
    }

    BuildGuidDataHob (&gAppleSiliconPkgPreloadedRamdiskDecisionGuid, &RamdiskDecision, sizeof (RamdiskDecision));
  }

  //
  // Declare the low DRAM window as usable system memory, and take its backing store out
  // of the normal map so the same physical pages are not handed out twice under two
  // different addresses.
  //
  if (PcdGet32 (PcdLowMemoryWindowSize) != 0) {
    DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: low DRAM window 0x%llx + 0x%x, backed by 0x%llx\n",
            PcdGet64 (PcdLowMemoryWindowBase), PcdGet32 (PcdLowMemoryWindowSize),
            PcdGet64 (PcdLowMemoryWindowBackingBase)));

    ReserveMemoryRegion (
      PcdGet64 (PcdLowMemoryWindowBackingBase),
      PcdGet32 (PcdLowMemoryWindowSize)
      );

    BuildResourceDescriptorHob (
      EFI_RESOURCE_SYSTEM_MEMORY,
      ResourceAttributes,
      PcdGet64 (PcdLowMemoryWindowBase),
      PcdGet32 (PcdLowMemoryWindowSize)
      );
  }

  // m1n1 selects the interval from the live guest layout and proves each
  // stage-2 leaf. Mu owns the OS map: revalidate every known PEI reservation
  // before punching the HOB hole. An invalid published receipt is fatal,
  // because ACPI would otherwise assign pages still usable by DXE/Windows.
#ifdef AGX_LOCAL_RESERVE_V2
  // New AML must never run on a missing, old, or unpublished receipt.
  if (MmioRead32 (AGX_LOCAL_BROKER_BASE) != AGX_LOCAL_BROKER_MAGIC ||
      MmioRead32 (AGX_LOCAL_BROKER_BASE + 4) != AGX_LOCAL_BROKER_VERSION ||
      MmioRead32 (AGX_LOCAL_BROKER_BASE + 8) != 1) {
    DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: reserve v2 receipt required\n"));
    return EFI_INCOMPATIBLE_VERSION;
  }
#endif
  if (MmioRead32 (AGX_LOCAL_BROKER_BASE) == AGX_LOCAL_BROKER_MAGIC &&
      MmioRead32 (AGX_LOCAL_BROKER_BASE + 4) == AGX_LOCAL_BROKER_VERSION &&
      MmioRead32 (AGX_LOCAL_BROKER_BASE + 8) == 1) {
    LocalBase = MmioRead64 (AGX_LOCAL_BROKER_BASE + 0x10);
    LocalHostBase = MmioRead64 (AGX_LOCAL_BROKER_BASE + 0x18);
    LocalBytes = MmioRead64 (AGX_LOCAL_BROKER_BASE + 0x20);
    LocalExcludedCount = 0;
    LocalExcluded[LocalExcludedCount++] = (AGX_LOCAL_RANGE){UefiMemoryBase, UefiMemorySize};
    if (FrameBufferSize != 0) {
      LocalExcluded[LocalExcludedCount++] = (AGX_LOCAL_RANGE){FrameBufferBase, FrameBufferSize};
    }
    if (PcdGet32 (PcdFdSize) != 0) {
      LocalExcluded[LocalExcludedCount++] = (AGX_LOCAL_RANGE){PcdGet64 (PcdFdBaseAddress),
                                                        PcdGet32 (PcdFdSize)};
    }
    if (PcdGet32 (PcdPreloadedRamdiskMaxSize) != 0) {
      LocalExcluded[LocalExcludedCount++] = (AGX_LOCAL_RANGE){PcdGet64 (PcdPreloadedRamdiskBase),
                                                        PcdGet32 (PcdPreloadedRamdiskMaxSize)};
    }
    if (PcdGet32 (PcdLowMemoryWindowSize) != 0) {
      LocalExcluded[LocalExcludedCount++] = (AGX_LOCAL_RANGE){PcdGet64 (PcdLowMemoryWindowBackingBase),
                                                        PcdGet32 (PcdLowMemoryWindowSize)};
    }
    if (LocalBytes != AGX_LOCAL_RESERVE_BYTES || LocalHostBase == 0 ||
        (LocalHostBase & (AGX_LOCAL_RESERVE_ALIGNMENT - 1ULL)) != 0 ||
        LocalHostBase != LocalBase ||
        !AgxLocalReserveHobsSafe (LocalBase) ||
        !AgxLocalReserveRangeValid (LocalBase, LocalBytes,
                                    PcdGet64 (PcdSystemMemoryBase),
                                    PcdGet64 (PcdSystemMemorySize),
                                    LocalExcluded, LocalExcludedCount) ||
        !AgxLocalReserveHobContains (LocalBase)) {
      DEBUG ((DEBUG_ERROR, "MemoryInitPeiLib: invalid AGX local reserve receipt\n"));
      return EFI_INVALID_PARAMETER;
    }
    DEBUG ((DEBUG_INFO, "MemoryInitPeiLib: AGX local reserve 0x%llx + 0x%llx PA=0x%llx\n",
            LocalBase, LocalBytes, LocalHostBase));
    ReserveMemoryRegion (LocalBase, (UINT32)AGX_LOCAL_RESERVE_BYTES);
    BuildMemoryAllocationHob (LocalBase, AGX_LOCAL_RESERVE_BYTES, EfiReservedMemoryType);
  }

  // Build Memory Allocation Hob
  InitMmu (MemoryTable);

  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    // Optional feature that helps prevent EFI memory map fragmentation.
    BuildMemoryTypeInformationHob ();
  }

  return EFI_SUCCESS;
}

/**
 * BuildVirtualMemoryMap
 * 
 * This will build up the memory map of the platform used to initialize the MMU and page tables.
 * 
 * @param VirtualMemoryMap - A pointer to a pointer to be used for page table setup.
 * 
 * does not return anything as the value will be stored in a pointer accessible by MemoryPeim.
 * 
 */
VOID BuildVirtualMemoryMap(OUT ARM_MEMORY_REGION_DESCRIPTOR **VirtualMemoryMap)
{
  ARM_MEMORY_REGION_ATTRIBUTES CacheAttributes;
  UINTN Index = 0;
  ARM_MEMORY_REGION_DESCRIPTOR *VirtualMemoryTable;

  //ensure we actually have a valid memory map pointer
  ASSERT(VirtualMemoryMap != NULL);

  DEBUG((DEBUG_INFO, "Allocating virtual memory table pages\n"));
  VirtualMemoryTable = (ARM_MEMORY_REGION_DESCRIPTOR *)AllocatePages (EFI_SIZE_TO_PAGES (sizeof (ARM_MEMORY_REGION_DESCRIPTOR) * MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS));
  if (VirtualMemoryTable == NULL) {
    DEBUG((DEBUG_INFO, "Unexpected failure to allocate VirtualMemoryTable\n"));
    return;
  }

  CacheAttributes = DDR_ATTRIBUTES_CACHED;

  /**
   * NOTE - On Apple silicon platforms, non PCIe MMIO regions *must* use nGnRnE mappings, 
   * while all PCIe regions *must* use nGnRE mappings.
   * by default EDK2 sets up the MMIO as nGnRnE, good for core system devices
   * though we will need to add an attribute for nGnRE mappings at some point.
   * 
   * TODO: add ARM_MEMORY_REGION_ATTRIBUTE_DEVICE_POSTED_WRITE
   **/

  //MMIO - PMGR/AIC/Core System Peripherals and PCIe
  VirtualMemoryTable[Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].VirtualBase  = APPLE_CORE_SYSTEM_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].Length       = APPLE_CORE_SYSTEM_MMIO_RANGE_1_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // Full-owner m1n1 exposes the local-memory handoff in an unused read-only
  // subwindow of the synthetic broker. PEI and ACPI read the same values.
  VirtualMemoryTable[++Index].PhysicalBase = AGX_LOCAL_BROKER_BASE & ~0xFFFULL;
  VirtualMemoryTable[Index].VirtualBase    = AGX_LOCAL_BROKER_BASE & ~0xFFFULL;
  VirtualMemoryTable[Index].Length         = 0x1000;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_2_SIZE;
  VirtualMemoryTable[Index].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_1_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_1_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_2_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_2_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_3_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_3_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_3_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_4_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_4_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_4_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_5_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_5_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_5_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_6_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_6_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_6_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_PCIE_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_PCIE_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_PCIE_MMIO_RANGE_7_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  VirtualMemoryTable[++Index].PhysicalBase = APPLE_CORE_SYSTEM_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].VirtualBase    = APPLE_CORE_SYSTEM_MMIO_RANGE_7_BASE;
  VirtualMemoryTable[Index].Length         = APPLE_CORE_SYSTEM_MMIO_RANGE_7_SIZE;
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  //System DRAM
  VirtualMemoryTable[++Index].PhysicalBase = PcdGet64(PcdSystemMemoryBase);
  VirtualMemoryTable[Index].VirtualBase    = PcdGet64(PcdSystemMemoryBase);
  VirtualMemoryTable[Index].Length         = PcdGet64(PcdSystemMemorySize);
  VirtualMemoryTable[Index].Attributes     = CacheAttributes;

  //
  // Low DRAM window. Apple DRAM starts at 0x800000000, but the Windows boot manager asks
  // for pages at low physical addresses (observed: a single page at 0x102000, after which
  // it returns EFI_INVALID_PARAMETER). The hypervisor aliases this range onto real memory
  // via stage-2, so from here it is ordinary cached DRAM.
  //
  if (PcdGet32 (PcdLowMemoryWindowSize) != 0) {
    VirtualMemoryTable[++Index].PhysicalBase = PcdGet64 (PcdLowMemoryWindowBase);
    VirtualMemoryTable[Index].VirtualBase    = PcdGet64 (PcdLowMemoryWindowBase);
    VirtualMemoryTable[Index].Length         = PcdGet32 (PcdLowMemoryWindowSize);
    VirtualMemoryTable[Index].Attributes     = CacheAttributes;
  }


  DEBUG ((
    DEBUG_ERROR,
    "%a: Dumping System DRAM Memory Map:\n"
    "\tPhysicalBase: 0x%lX\n"
    "\tVirtualBase: 0x%lX\n"
    "\tLength: 0x%lX\n"
    "\tTop of system RAM: 0x%lX\n",
    __FUNCTION__,
    VirtualMemoryTable[Index].PhysicalBase,
    VirtualMemoryTable[Index].VirtualBase,
    VirtualMemoryTable[Index].Length,
    VirtualMemoryTable[Index].PhysicalBase + VirtualMemoryTable[Index].Length
    ));

  //Framebuffer
  VirtualMemoryTable[++Index].PhysicalBase = PcdGet64(PcdFrameBufferAddress);
  VirtualMemoryTable[Index].VirtualBase    = PcdGet64(PcdFrameBufferAddress);
  VirtualMemoryTable[Index].Length         = PcdGet64(PcdFrameBufferSize);
  VirtualMemoryTable[Index].Attributes     = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;

  // End of Table
  VirtualMemoryTable[++Index].PhysicalBase  = 0;
  VirtualMemoryTable[Index].VirtualBase     = 0;
  VirtualMemoryTable[Index].Length          = 0;
  VirtualMemoryTable[Index].Attributes      = (ARM_MEMORY_REGION_ATTRIBUTES)0;

  ASSERT((Index + 1) <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);

  *VirtualMemoryMap = VirtualMemoryTable;
}

/**
 * @file BootRamdiskHelperDxe.c
 * @author amarioguy (Arminder Singh)
 * 
 * Sets up an embedded RAMDisk in the FV. For right now this is primarily used to bring up WinPE.
 * 
 * @version 1.0
 * @date 2022-12-25
 * 
 * @copyright Copyright (c) amarioguy (Arminder Singh), 2022.
 * 
 */

#include <PiDxe.h>

#include "BootRamdiskHelperDxe.h"

/**
 * @brief Main function for RAMDisk initialization.
 * 
 * Note that we only allocate a RAMDisk with a boot image here if we are told to do so and actually have the file embedded in the FV.
 * 
 * @param ImageHandle 
 * @param SystemTable 
 * @return
 * 
 * EFI_SUCCESS - we initialized the RAMDisk as a bootable device.
 * EFI_UNSUPPORTED - we are configured not to set up the RAMDisk.
 * EFI_NOT_FOUND - no FV embedded candidate image is present.
 * EFI_OUT_OF_RESOURCES - for some reason, we are out of memory and cannot create the ramdisk.
 * EFI_ABORTED - an unexpected error occurred.
 */
EFI_STATUS
EFIAPI
BootRamdiskHelperDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
    EFI_STATUS Status;
    BOOLEAN RamdiskBootConfigured;
    VOID *OriginalRamDiskPtr;
    VOID *DestinationRamdiskPtr;
    UINTN RamDiskSize;
    EFI_GUID *RamDiskRegisterType = &gEfiVirtualDiskGuid; //hardcode to IMG image for now
    EFI_RAM_DISK_PROTOCOL *RamdiskProtocol;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;

    DEBUG((DEBUG_INFO, "BootRamdiskHelperDxe started\n"));
    // Before proceeding to RAMDisk creation, check that we're configured to do so
    // and that we have a candidate image with which to create said RAMDisk.
    RamdiskBootConfigured = PcdGetBool(PcdInitializeRamdisk);
    if(!RamdiskBootConfigured) {
        DEBUG((DEBUG_ERROR, "BootRamdiskHelperDxe - FV not configured for ramdisk boot, exiting\n"));
        return EFI_UNSUPPORTED;
    }

    //
    // Prefer an image preloaded into guest RAM over one embedded in the firmware volume.
    // FvMain is decompressed in its entirety during PrePi, so anything past a few tens of
    // megabytes dies there with Out of Resources - a WinPE image cannot travel that way.
    // The window is carved out of the memory map in MemoryInitPeiLib, so the image can be
    // handed to the RAMDisk protocol in place, with no copy.
    //
    if(PcdGet64(PcdPreloadedRamdiskBase) != 0) {
        PRELOADED_RAMDISK_HEADER *Header = (VOID *)(UINTN)PcdGet64(PcdPreloadedRamdiskBase);

        if(CompareMem(Header->Magic, PRELOADED_RAMDISK_MAGIC, sizeof(Header->Magic)) == 0) {
            UINT64 PayloadBase = PcdGet64(PcdPreloadedRamdiskBase) + PRELOADED_RAMDISK_PAYLOAD_OFFSET;

            if((Header->Size == 0) ||
               (Header->Size > PcdGet32(PcdPreloadedRamdiskMaxSize) - PRELOADED_RAMDISK_PAYLOAD_OFFSET)) {
                DEBUG((DEBUG_ERROR, "BootRamdiskHelperDxe: preloaded RAMDisk size 0x%llx is out of range\n", Header->Size));
                return EFI_VOLUME_CORRUPTED;
            }

            DEBUG((DEBUG_ERROR, "BootRamdiskHelperDxe: preloaded RAMDisk at 0x%llx, 0x%llx bytes\n",
                   PayloadBase, Header->Size));

            Status = gBS->LocateProtocol(&gEfiRamDiskProtocolGuid, NULL, (VOID **)&RamdiskProtocol);
            if (EFI_ERROR (Status)) {
                DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: Couldn't find the RAMDisk protocol - %r\n", Status));
                return Status;
            }

            Status = RamdiskProtocol->Register(PayloadBase, Header->Size, RamDiskRegisterType, NULL, &DevicePath);
            if (EFI_ERROR (Status)) {
                DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: Cannot register preloaded RAM Disk - %r\n", Status));
            }

            return Status;
        }

        DEBUG((DEBUG_ERROR, "BootRamdiskHelperDxe: no preloaded RAMDisk at 0x%llx, falling back to the FV\n",
               PcdGet64(PcdPreloadedRamdiskBase)));
    }
    Status = GetSectionFromAnyFv(&gAppleSiliconPkgEmbeddedRamdiskGuid, EFI_SECTION_RAW, 0, &OriginalRamDiskPtr, &RamDiskSize);
    if(EFI_ERROR(Status)) {
        DEBUG((DEBUG_ERROR, "BootRamdiskHelperDxe - no FV embedded ramdisk, exiting\n"));
        return EFI_NOT_FOUND;
    }

    ASSERT (OriginalRamDiskPtr != NULL);
    ASSERT (RamDiskSize != 0);
    //copy the RAMDisk to a new scratch location
    DestinationRamdiskPtr = AllocateCopyPool(RamDiskSize, OriginalRamDiskPtr);

    ASSERT (DestinationRamdiskPtr != NULL);

    Status = gBS->LocateProtocol(&gEfiRamDiskProtocolGuid, NULL, (VOID **)&RamdiskProtocol);
    if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: Couldn't find the RAMDisk protocol - %r\n", Status));
        return Status;
    }
    Status = RamdiskProtocol->Register((UINTN)DestinationRamdiskPtr, (UINT64)RamDiskSize, RamDiskRegisterType, NULL, &DevicePath);
    if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "BootRamdiskHelperDxe: Cannot register RAM Disk - %r\n", Status));
    }

    return Status;

}

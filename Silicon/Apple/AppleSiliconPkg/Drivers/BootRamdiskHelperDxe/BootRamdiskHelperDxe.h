/**
 * @file BootRamdiskHelperDxe.h
 * @author amarioguy (Arminder Singh)
 * 
 * Boot RAMDisk Helper DXE Driver header file
 * @version 1.0
 * @date 2022-12-25
 * 
 * @copyright Copyright (c) amarioguy (Arminder Singh), 2022.
 * 
 */

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiHiiServicesLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/HiiLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/ComponentName.h>
#include <Protocol/RamDisk.h>
#include <Protocol/HiiConfigAccess.h>

//
// A RAMDisk image preloaded into guest RAM by the loader (run_uefi.py) rather than
// embedded in the firmware volume: header ABI and PEI decision HOB.
//
#include <Library/PreloadedRamdisk.h>
#include <Library/HobLib.h>

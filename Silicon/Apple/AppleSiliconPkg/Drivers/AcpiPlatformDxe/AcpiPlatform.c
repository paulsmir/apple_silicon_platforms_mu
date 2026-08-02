/** @file
 * Copyright (c) 2023, amarioguy (AppleWOA authors).
 * 
 * Module Name:
 *  AcpiPlatformDxe.c
 * 
 * Abstract:
 *  ACPI platform driver. Installs ACPI tables for the platform (device specific and SoC general)
 *  Based off the sample driver in MdeModulePkg.
 * 
 * Environment:
 *  UEFI Driver Execution Environment (DXE)/UEFI boot services
 * 
 * License:
 *  SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 * 
**/

#include <PiDxe.h>

#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <IndustryStandard/Acpi.h>
#include <Guid/Acpi.h>

//
// Dump the LIVE ACPI chain (RSDP -> XSDT -> table list) at ReadyToBoot, i.e. exactly what the
// OS will find. This is the measurement that replaces a proxy RAM scan (which resets this
// machine) and kd: it says directly whether MCFG reached the XSDT the guest sees, and prints
// the RSDP address so kd_acpi.py can cross-check from inside Windows.
//
STATIC
VOID
AcpiDumpConfigTable (
  VOID
  )
{
  UINTN                        Index;
  VOID                         *Rsdp = NULL;
  EFI_ACPI_DESCRIPTION_HEADER  *Xsdt;
  UINT64                       XsdtAddr;
  UINT64                       *Entries;
  UINTN                        Count;

  //
  // Print every configuration-table entry (GUID first dword + VendorTable pointer). This alone
  // yields the RSDP address for kd_acpi.py and cannot fault - no table contents are touched.
  //
  DEBUG ((DEBUG_ERROR, "ACPI LIVE: %u configuration tables\n",
          (UINT32)gST->NumberOfTableEntries));
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    DEBUG ((DEBUG_ERROR, "ACPI LIVE: cfg[%u] guid0=0x%08x table=0x%lx\n", (UINT32)Index,
            *(UINT32 *)&gST->ConfigurationTable[Index].VendorGuid,
            (UINT64)(UINTN)gST->ConfigurationTable[Index].VendorTable));
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpiTableGuid)) {
      Rsdp = gST->ConfigurationTable[Index].VendorTable;
    }
  }

  DEBUG ((DEBUG_ERROR, "ACPI LIVE: RSDP @ 0x%lx\n", (UINT64)(UINTN)Rsdp));
  if (Rsdp == NULL) {
    DEBUG ((DEBUG_ERROR, "ACPI LIVE: no ACPI 2.0 table in the configuration table!\n"));
    return;
  }

  //
  // Validate before dereferencing: an earlier version walked the XSDT unconditionally and took
  // a synchronous exception in ArmCpuDxe, killing the boot. Print the RSDP address (enough for
  // kd_acpi.py to cross-check from inside Windows) and only walk when the data looks sane.
  //
  if (CompareMem (Rsdp, "RSD PTR ", 8) != 0) {
    DEBUG ((DEBUG_ERROR, "ACPI LIVE: RSDP signature mismatch, not walking\n"));
    return;
  }

  XsdtAddr = *(UINT64 *)((UINT8 *)Rsdp + 24);
  DEBUG ((DEBUG_ERROR, "ACPI LIVE: XSDT @ 0x%lx\n", XsdtAddr));
  if (XsdtAddr == 0) {
    return;
  }

  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)XsdtAddr;
  if (Xsdt->Signature != SIGNATURE_32 ('X', 'S', 'D', 'T') ||
      Xsdt->Length < sizeof (EFI_ACPI_DESCRIPTION_HEADER) || Xsdt->Length > 0x1000) {
    DEBUG ((DEBUG_ERROR, "ACPI LIVE: XSDT header implausible (sig=0x%x len=0x%x)\n",
            Xsdt->Signature, Xsdt->Length));
    return;
  }

  Count   = (Xsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / sizeof (UINT64);
  Entries = (UINT64 *)((UINT8 *)Xsdt + sizeof (EFI_ACPI_DESCRIPTION_HEADER));

  for (Index = 0; Index < Count && Index < 32; Index++) {
    UINT64  TblAddr = Entries[Index];
    UINT32  Sig;

    if (TblAddr == 0) {
      continue;
    }
    Sig = ((EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)TblAddr)->Signature;
    DEBUG ((DEBUG_ERROR, "ACPI LIVE: XSDT[%u] %c%c%c%c @0x%lx\n",
            (UINT32)Index, (CHAR8)Sig, (CHAR8)(Sig >> 8), (CHAR8)(Sig >> 16),
            (CHAR8)(Sig >> 24), TblAddr));
  }
}
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>

#include <IndustryStandard/Acpi.h>

/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the device-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithDeviceTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **DeviceInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdDeviceAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *DeviceInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}


/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the device family-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithDeviceFamilyTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **DeviceInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdDeviceFamilyAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *DeviceInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}



/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the SoC-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithSocTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **SocInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdSocAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *SocInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}

/**
  Locate the first instance of a protocol.  If the protocol requested is an
  FV protocol, then it will return the first FV that contains the SoC-specific ACPI table
  storage file.

  @param  Instance      Return pointer to the first instance of the protocol

  @return EFI_SUCCESS           The function completed successfully.
  @return EFI_NOT_FOUND         The protocol could not be located.
  @return EFI_OUT_OF_RESOURCES  There are not enough resources to find the protocol.

**/
EFI_STATUS
LocateFvInstanceWithGenericTables (
  OUT EFI_FIRMWARE_VOLUME2_PROTOCOL  **GenericInstance
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          NumberOfHandles;
  EFI_FV_FILETYPE                FileType;
  UINT32                         FvStatus;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINTN                          Size;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FvInstance;

  FvStatus = 0;

  //
  // Locate protocol.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &NumberOfHandles,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    //
    // Defined errors at this time are not found and out of resources.
    //
    return Status;
  }

  //
  // Looking for FV with device-specific ACPI table storage file
  //

  for (Index = 0; Index < NumberOfHandles; Index++) {
    //
    // Get the protocol on this handle
    // This should not fail because of LocateHandleBuffer
    //
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&FvInstance
                    );
    ASSERT_EFI_ERROR (Status);

    //
    // See if it has the ACPI storage file
    //
    Status = FvInstance->ReadFile (
                           FvInstance,
                           (EFI_GUID *)PcdGetPtr (PcdGenericAcpiTableStorageFile),
                           NULL,
                           &Size,
                           &FileType,
                           &Attributes,
                           &FvStatus
                           );

    //
    // If we found it, then we are done
    //
    if (Status == EFI_SUCCESS) {
      *GenericInstance = FvInstance;
      break;
    }
  }

  //
  // Our exit status is determined by the success of the previous operations
  // If the protocol was found, Instance already points to it.
  //

  //
  // Free any allocated buffers
  //
  gBS->FreePool (HandleBuffer);

  return Status;
}

/**
  This function calculates and updates an UINT8 checksum.

  @param  Buffer          Pointer to buffer to checksum
  @param  Size            Number of bytes to checksum

**/
VOID
AcpiPlatformChecksum (
  IN UINT8  *Buffer,
  IN UINTN  Size
  )
{
  UINTN  ChecksumOffset;

  ChecksumOffset = OFFSET_OF (EFI_ACPI_DESCRIPTION_HEADER, Checksum);

  //
  // Set checksum to 0 first
  //
  Buffer[ChecksumOffset] = 0;

  //
  // Update checksum value
  //
  Buffer[ChecksumOffset] = CalculateCheckSum8 (Buffer, Size);
}

// EFI_STATUS AcpiPlatformInstallMadtTable(VOID) {
//   //
//   // We need to install an MADT - we can use the same table regardless of
//   // whether we're using a vGIC or not.
//   //
  
//   return EFI_UNSUPPORTED;
// }

/**
  Entrypoint of Acpi Platform driver.

  @param  ImageHandle
  @param  SystemTable

  @return EFI_SUCCESS
  @return EFI_LOAD_ERROR
  @return EFI_OUT_OF_RESOURCES

**/
EFI_STATUS
EFIAPI
AcpiPlatformEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  EFI_ACPI_TABLE_PROTOCOL        *AcpiTable;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FwVol;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *FwVol2;
  INTN                           Instance;
  EFI_ACPI_COMMON_HEADER         *CurrentTable;
  UINTN                          TableHandle;
  UINT32                         FvStatus;
  UINTN                          TableSize;
  UINTN                          Size;

  Instance     = 0;
  CurrentTable = NULL;
  TableHandle  = 0;

  DEBUG((DEBUG_ERROR, "%a: AcpiPlatform driver started\n", __FUNCTION__));

  //
  // Register the emulated-NVMe ECAM (0x690000000, 1 MB) as MMIO in the GCD so it lands in the
  // UEFI memory map and Windows can map the segment-0 config space. Nothing else adds it:
  // PciHostBridgeDxe only adds the BAR MMIO apertures, and AppleSiliconPciPlatformDxe (which
  // reads the ADT apcie ECAM) is not in the FDF. The %r is also a probe - "Access Denied" =>
  // the region was already present (hypothesis wrong), "Success" => it was missing.
  //
  {
    EFI_STATUS  GcdStatus = gDS->AddMemorySpace (
                                   EfiGcdMemoryTypeMemoryMappedIo,
                                   0x690000000, 0x100000, EFI_MEMORY_UC);
    DEBUG ((DEBUG_ERROR, "ECAM GCD AddMemorySpace(0x690000000,0x100000) = %r\n", GcdStatus));
    if (!EFI_ERROR (GcdStatus)) {
      gDS->SetMemorySpaceAttributes (0x690000000, 0x100000, EFI_MEMORY_UC);
    }
  }

  //
  // NOTE: an earlier version ran this dump from a ReadyToBoot event; the callback faulted
  // immediately (synchronous exception in ArmCpuDxe, boot dead) before any output flushed.
  // The dump is now called synchronously at the end of this entry point instead - see the
  // call after the tables are installed.
  //

  //
  // Find the AcpiTable protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating ACPI table protocol\n", __FUNCTION__));
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate ACPI protocol, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Locate the firmware volume protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating device specific ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithDeviceTables (&FwVol);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate device specific tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol->ReadSection (
                      FwVol,
                      (EFI_GUID *)PcdGetPtr (PcdDeviceAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // DEBUG: log every table actually installed into the XSDT, so the firmware log shows
      // directly whether MCFG travels (vs. only living in the source). Answers "did the table
      // reach the live XSDT" deterministically, without kd or a RAM scan.
      //
      {
        UINT32  DbgSig = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Signature;
        DEBUG ((DEBUG_ERROR, "ACPI INSTALL instance=%u sig=%c%c%c%c len=0x%x\n",
                Instance, (CHAR8)DbgSig, (CHAR8)(DbgSig >> 8),
                (CHAR8)(DbgSig >> 16), (CHAR8)(DbgSig >> 24), TableSize));
      }

      //
      // Checksum ACPI table
      //
      AcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }

  Instance     = 0;
  CurrentTable = NULL;
  TableHandle  = 0;
  //
  // Locate the firmware volume protocol
  //
  DEBUG((DEBUG_ERROR, "%a: Locating SoC specific ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithSocTables (&FwVol2);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate SoC specific tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol2->ReadSection (
                      FwVol2,
                      (EFI_GUID *)PcdGetPtr (PcdSocAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // DEBUG: log every table actually installed into the XSDT, so the firmware log shows
      // directly whether MCFG travels (vs. only living in the source). Answers "did the table
      // reach the live XSDT" deterministically, without kd or a RAM scan.
      //
      {
        UINT32  DbgSig = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Signature;
        DEBUG ((DEBUG_ERROR, "ACPI INSTALL instance=%u sig=%c%c%c%c len=0x%x\n",
                Instance, (CHAR8)DbgSig, (CHAR8)(DbgSig >> 8),
                (CHAR8)(DbgSig >> 16), (CHAR8)(DbgSig >> 24), TableSize));
      }

      //
      // Checksum ACPI table
      //
      AcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }


  //
  // Locate the firmware volume protocol
  //
  //
  // Section indices are per-file, so every block must start counting at zero. The device
  // and SoC blocks reset it; these last two did not, and inherited the previous block's
  // final count - silently skipping that many of their own sections. On this platform the
  // family file holds exactly one table, the FADT, which was therefore never installed.
  //
  Instance     = 0;
  CurrentTable = NULL;
  TableHandle  = 0;

  DEBUG((DEBUG_ERROR, "%a: Locating generic ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithGenericTables (&FwVol2);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate generic tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol2->ReadSection (
                      FwVol2,
                      (EFI_GUID *)PcdGetPtr (PcdGenericAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // DEBUG: log every table actually installed into the XSDT, so the firmware log shows
      // directly whether MCFG travels (vs. only living in the source). Answers "did the table
      // reach the live XSDT" deterministically, without kd or a RAM scan.
      //
      {
        UINT32  DbgSig = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Signature;
        DEBUG ((DEBUG_ERROR, "ACPI INSTALL instance=%u sig=%c%c%c%c len=0x%x\n",
                Instance, (CHAR8)DbgSig, (CHAR8)(DbgSig >> 8),
                (CHAR8)(DbgSig >> 16), (CHAR8)(DbgSig >> 24), TableSize));
      }

      //
      // Checksum ACPI table
      //
      AcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }

  //
  // Locate the firmware volume protocol
  //
  Instance     = 0;
  CurrentTable = NULL;
  TableHandle  = 0;

  DEBUG((DEBUG_ERROR, "%a: Locating device family ACPI tables\n", __FUNCTION__));
  Status = LocateFvInstanceWithDeviceFamilyTables (&FwVol2);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "%a: Failed to locate device family tables, status %r (error code %llx)\n", __FUNCTION__, Status, Status));
    ASSERT(FALSE);
    return EFI_ABORTED;
  }

  //
  // Read tables from the storage file.
  //
  while (Status == EFI_SUCCESS) {
    Status = FwVol2->ReadSection (
                      FwVol2,
                      (EFI_GUID *)PcdGetPtr (PcdDeviceFamilyAcpiTableStorageFile),
                      EFI_SECTION_RAW,
                      Instance,
                      (VOID **)&CurrentTable,
                      &Size,
                      &FvStatus
                      );
    if (!EFI_ERROR (Status)) {
      //
      // Add the table
      //
      TableHandle = 0;

      TableSize = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Length;
      ASSERT (Size >= TableSize);

      //
      // DEBUG: log every table actually installed into the XSDT, so the firmware log shows
      // directly whether MCFG travels (vs. only living in the source). Answers "did the table
      // reach the live XSDT" deterministically, without kd or a RAM scan.
      //
      {
        UINT32  DbgSig = ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable)->Signature;
        DEBUG ((DEBUG_ERROR, "ACPI INSTALL instance=%u sig=%c%c%c%c len=0x%x\n",
                Instance, (CHAR8)DbgSig, (CHAR8)(DbgSig >> 8),
                (CHAR8)(DbgSig >> 16), (CHAR8)(DbgSig >> 24), TableSize));
      }

      //
      // Checksum ACPI table
      //
      AcpiPlatformChecksum ((UINT8 *)CurrentTable, TableSize);

      //
      // Install ACPI table
      //
      Status = AcpiTable->InstallAcpiTable (
                            AcpiTable,
                            CurrentTable,
                            TableSize,
                            &TableHandle
                            );

      //
      // Free memory allocated by ReadSection
      //
      gBS->FreePool (CurrentTable);

      if (EFI_ERROR (Status)) {
        return EFI_ABORTED;
      }

      //
      // Increment the instance
      //
      Instance++;
      CurrentTable = NULL;
    }
  }

  //
  // Dynamically generate and install the MADT table.
  // We have to do this because we will only know the number of cores
  // (which is needed to allocate for redistributors correctly) at runtime.
  //

  // Temporarily disabled - using a static MADT for now

  // Status = AcpiPlatformInstallMadtTable();


  //
  // All tables are installed by now: dump the live ACPI chain (measurement - does MCFG reach
  // the XSDT the OS will read?). Synchronous, so a fault here is attributable and not silent.
  //
  AcpiDumpConfigTable ();

  //
  // The driver does not require to be kept loaded.
  //
  return EFI_REQUEST_UNLOAD_IMAGE;
}

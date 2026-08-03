/** @file
  Deterministically launch the installed Windows fallback loader from internal storage.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#define WINDOWS_FALLBACK_LOADER  L"\\EFI\\BOOT\\BOOTAA64.EFI"

STATIC EFI_HANDLE  mDriverImageHandle;
STATIC EFI_EVENT   mReadyToBootEvent;
STATIC EFI_EVENT   mSimpleFileSystemEvent;
STATIC VOID        *mSimpleFileSystemRegistration;
STATIC BOOLEAN     mWindowsStarted;

STATIC
VOID
ConnectAllBlockIoHandles (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;

  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "WindowsAutoBootDxe: no BlockIo handles: %r\n", Status));
    return;
  }

  // At the first ReadyToBoot signal BDS has not necessarily connected partition/FAT
  // children of the emulated NVMe yet.  The internal ESP therefore has no SimpleFS
  // handle until its BlockIo ancestry is recursively connected.
  for (UINTN Index = 0; Index < HandleCount; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
}

STATIC
BOOLEAN
HasWindowsFallbackLoader (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;

  Root = NULL;
  File = NULL;
  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Status = Root->Open (
                   Root,
                   &File,
                   WINDOWS_FALLBACK_LOADER,
                   EFI_FILE_MODE_READ,
                   0
                   );
  if (!EFI_ERROR (Status)) {
    File->Close (File);
  }

  Root->Close (Root);
  return !EFI_ERROR (Status);
}

STATIC
EFI_STATUS
StartWindowsFromHandle (
  IN EFI_HANDLE  FileSystemHandle
  )
{
  EFI_STATUS                 Status;
  EFI_DEVICE_PATH_PROTOCOL   *LoaderPath;
  EFI_HANDLE                 LoaderImage;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;

  LoaderPath = FileDevicePath (FileSystemHandle, WINDOWS_FALLBACK_LOADER);
  if (LoaderPath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  LoaderImage = NULL;
  Status = gBS->LoadImage (
                  FALSE,
                  mDriverImageHandle,
                  LoaderPath,
                  NULL,
                  0,
                  &LoaderImage
                  );
  FreePool (LoaderPath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LoadedImage = NULL;
  Status = gBS->HandleProtocol (
                  LoaderImage,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status)) {
    gBS->UnloadImage (LoaderImage);
    return Status;
  }

  // Shell/BDS load options make bootmgfw reject an otherwise valid device-path launch.
  LoadedImage->LoadOptions = NULL;
  LoadedImage->LoadOptionsSize = 0;

  DEBUG ((DEBUG_ERROR, "WindowsAutoBootDxe: starting %s\n", WINDOWS_FALLBACK_LOADER));
  Status = gBS->StartImage (LoaderImage, NULL, NULL);
  DEBUG ((DEBUG_ERROR, "WindowsAutoBootDxe: StartImage returned %r\n", Status));
  gBS->UnloadImage (LoaderImage);
  return Status;
}

STATIC
BOOLEAN
TryStartWindowsFromHandle (
  IN EFI_HANDLE  FileSystemHandle
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_BLOCK_IO_PROTOCOL            *BlockIo;

  if (mWindowsStarted) {
    return TRUE;
  }

  BlockIo = NULL;
  Status = gBS->HandleProtocol (
                  FileSystemHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo
                  );
  if (EFI_ERROR (Status) || (BlockIo->Media == NULL) || !BlockIo->Media->MediaPresent ||
      BlockIo->Media->RemovableMedia) {
    return FALSE;
  }

  FileSystem = NULL;
  Status = gBS->HandleProtocol (
                  FileSystemHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR (Status) || !HasWindowsFallbackLoader (FileSystem)) {
    return FALSE;
  }

  mWindowsStarted = TRUE;
  Status = StartWindowsFromHandle (FileSystemHandle);
  if (EFI_ERROR (Status)) {
    mWindowsStarted = FALSE;
    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
EFIAPI
WindowsAutoBootFileSystemNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINTN       HandleSize;

  (VOID)Event;
  (VOID)Context;

  while (!mWindowsStarted) {
    HandleSize = sizeof (Handle);
    Status = gBS->LocateHandle (
                    ByRegisterNotify,
                    NULL,
                    mSimpleFileSystemRegistration,
                    &HandleSize,
                    &Handle
                    );
    if (EFI_ERROR (Status)) {
      break;
    }

    TryStartWindowsFromHandle (Handle);
  }
}

STATIC
VOID
EFIAPI
WindowsAutoBootReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;

  (VOID)Context;
  gBS->CloseEvent (Event);
  mReadyToBootEvent = NULL;

  ConnectAllBlockIoHandles ();

  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "WindowsAutoBootDxe: no filesystems: %r\n", Status));
    return;
  }

  DEBUG ((DEBUG_ERROR, "WindowsAutoBootDxe: checking %u filesystems\n", HandleCount));
  for (UINTN Index = 0; Index < HandleCount; Index++) {
    if (TryStartWindowsFromHandle (Handles[Index])) {
      break;
    }
  }

  FreePool (Handles);
}

EFI_STATUS
EFIAPI
WindowsAutoBootDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  (VOID)SystemTable;
  mDriverImageHandle = ImageHandle;
  mWindowsStarted = FALSE;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  WindowsAutoBootFileSystemNotify,
                  NULL,
                  &mSimpleFileSystemEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiSimpleFileSystemProtocolGuid,
                  mSimpleFileSystemEvent,
                  &mSimpleFileSystemRegistration
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mSimpleFileSystemEvent);
    return Status;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  WindowsAutoBootReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyToBootEvent
                  );
  DEBUG ((DEBUG_ERROR, "WindowsAutoBootDxe: ReadyToBoot hook %r\n", Status));
  return Status;
}

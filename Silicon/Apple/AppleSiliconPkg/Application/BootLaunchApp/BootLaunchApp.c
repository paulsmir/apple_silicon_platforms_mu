/**
 * @file BootLaunchApp.c
 *
 * Loads and starts an EFI image the way BDS would rather than the way the UEFI Shell does,
 * and traces the Boot Services calls it makes.
 *
 * Two reasons this exists:
 *
 * 1. The Shell hands the image it starts a LoadOptions string containing the command line
 *    it was typed with. The Windows boot manager reads LoadOptions as its own boot
 *    application options; BDS passes either nothing or a boot option's OptionalData, so
 *    this launcher clears LoadOptions to match.
 *
 * 2. bootmgfw.efi returns EFI_INVALID_PARAMETER without opening a single file, so the
 *    failure is somewhere in its early initialisation. Wrapping the Boot Services table
 *    around StartImage shows which call it dislikes.
 *
 * Tracing writes through DEBUG(), which goes straight out of SerialPortLib and does not
 * itself call Boot Services - wrapping gBS and then calling Print() from inside a wrapper
 * would recurse forever.
 *
 * Usage: BootLaunchApp.efi FS2:\EFI\BOOT\BOOTAA64.EFI [-q]
 *        -q traces only calls that fail.
 */

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>

STATIC BOOLEAN  mFailuresOnly = FALSE;

//
// Saved originals.
//
STATIC EFI_ALLOCATE_PAGES        mAllocatePages;
STATIC EFI_FREE_PAGES            mFreePages;
STATIC EFI_GET_MEMORY_MAP        mGetMemoryMap;
STATIC EFI_ALLOCATE_POOL         mAllocatePool;
STATIC EFI_HANDLE_PROTOCOL       mHandleProtocol;
STATIC EFI_LOCATE_PROTOCOL       mLocateProtocol;
STATIC EFI_OPEN_PROTOCOL         mOpenProtocol;
STATIC EFI_LOCATE_HANDLE_BUFFER  mLocateHandleBuffer;
STATIC EFI_LOCATE_DEVICE_PATH    mLocateDevicePath;
STATIC EFI_CREATE_EVENT          mCreateEvent;
STATIC EFI_SET_TIMER             mSetTimer;
STATIC EFI_WAIT_FOR_EVENT        mWaitForEvent;
STATIC EFI_SET_WATCHDOG_TIMER    mSetWatchdogTimer;
STATIC EFI_IMAGE_LOAD            mLoadImage;
STATIC EFI_IMAGE_START           mStartImage;
STATIC EFI_INSTALL_CONFIGURATION_TABLE mInstallConfigurationTable;
STATIC EFI_EXIT                  mExit;
STATIC EFI_FREE_POOL             mFreePool;
STATIC EFI_LOCATE_HANDLE         mLocateHandle;
STATIC EFI_STALL                 mStall;
STATIC EFI_CLOSE_EVENT           mCloseEvent;
STATIC EFI_SIGNAL_EVENT          mSignalEvent;
STATIC EFI_CHECK_EVENT           mCheckEvent;
STATIC EFI_CREATE_EVENT_EX       mCreateEventEx;
STATIC EFI_CLOSE_PROTOCOL        mCloseProtocol;
STATIC EFI_REGISTER_PROTOCOL_NOTIFY mRegisterProtocolNotify;
STATIC EFI_INSTALL_PROTOCOL_INTERFACE mInstallProtocolInterface;
STATIC EFI_CONNECT_CONTROLLER    mConnectController;
STATIC EFI_EXIT_BOOT_SERVICES    mExitBootServices;

//
// Runtime Services matter as much as Boot Services here: this platform has neither
// non-volatile variable storage nor a real RTC, and the Windows boot manager reads
// variables and asks for the time early on.
//
STATIC EFI_GET_VARIABLE           mGetVariable;
STATIC EFI_SET_VARIABLE           mSetVariable;
STATIC EFI_GET_NEXT_VARIABLE_NAME mGetNextVariableName;
STATIC EFI_QUERY_VARIABLE_INFO    mQueryVariableInfo;
STATIC EFI_GET_TIME               mGetTime;
STATIC EFI_SET_TIME               mSetTime;
STATIC EFI_GET_WAKEUP_TIME        mGetWakeupTime;

//
// Reentrancy guard. With a DebugLib that writes through gST->ConOut (the QEMU build), the
// logging inside a wrapper goes back through Boot Services and hits the wrappers again.
//
STATIC UINTN  mInTrace = 0;

#define TRACE(Name, Status, Fmt, ...)                                        \
  do {                                                                       \
    if ((mInTrace == 0) && (!mFailuresOnly || EFI_ERROR (Status))) {         \
      mInTrace++;                                                            \
      DEBUG ((DEBUG_ERROR, "BS: " Name " " Fmt " -> %r\n", ##__VA_ARGS__, Status)); \
      mInTrace--;                                                            \
    }                                                                        \
  } while (FALSE)

STATIC
EFI_STATUS
EFIAPI
TraceAllocatePages (
  IN EFI_ALLOCATE_TYPE Type, IN EFI_MEMORY_TYPE MemoryType,
  IN UINTN Pages, IN OUT EFI_PHYSICAL_ADDRESS *Memory
  )
{
  EFI_PHYSICAL_ADDRESS  Wanted = *Memory;
  EFI_STATUS            Status = mAllocatePages (Type, MemoryType, Pages, Memory);
  TRACE ("AllocatePages", Status, "type=%d memtype=%d pages=0x%lx addr=0x%llx got=0x%llx",
         Type, MemoryType, (UINT64)Pages, Wanted, *Memory);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceFreePages (
  IN EFI_PHYSICAL_ADDRESS Memory, IN UINTN Pages
  )
{
  EFI_STATUS  Status = mFreePages (Memory, Pages);
  TRACE ("FreePages", Status, "addr=0x%llx pages=0x%lx", Memory, (UINT64)Pages);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceGetMemoryMap (
  IN OUT UINTN *Size, IN OUT EFI_MEMORY_DESCRIPTOR *Map, OUT UINTN *Key,
  OUT UINTN *DescSize, OUT UINT32 *DescVer
  )
{
  EFI_STATUS  Status = mGetMemoryMap (Size, Map, Key, DescSize, DescVer);
  TRACE ("GetMemoryMap", Status, "size=0x%lx", (UINT64)*Size);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceAllocatePool (
  IN EFI_MEMORY_TYPE MemoryType, IN UINTN Size, OUT VOID **Buffer
  )
{
  EFI_STATUS  Status = mAllocatePool (MemoryType, Size, Buffer);
  TRACE ("AllocatePool", Status, "memtype=%d size=0x%lx", MemoryType, (UINT64)Size);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceHandleProtocol (
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface
  )
{
  EFI_STATUS  Status = mHandleProtocol (Handle, Protocol, Interface);
  TRACE ("HandleProtocol", Status, "handle=%p %g", Handle, Protocol);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceLocateProtocol (
  IN EFI_GUID *Protocol, IN VOID *Registration, OUT VOID **Interface
  )
{
  EFI_STATUS  Status = mLocateProtocol (Protocol, Registration, Interface);
  TRACE ("LocateProtocol", Status, "%g", Protocol);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceOpenProtocol (
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface OPTIONAL,
  IN EFI_HANDLE AgentHandle, IN EFI_HANDLE ControllerHandle, IN UINT32 Attributes
  )
{
  EFI_STATUS  Status = mOpenProtocol (Handle, Protocol, Interface, AgentHandle,
                                      ControllerHandle, Attributes);
  TRACE ("OpenProtocol", Status, "handle=%p %g attr=0x%x", Handle, Protocol, Attributes);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceLocateHandleBuffer (
  IN EFI_LOCATE_SEARCH_TYPE SearchType, IN EFI_GUID *Protocol OPTIONAL,
  IN VOID *SearchKey OPTIONAL, OUT UINTN *NoHandles, OUT EFI_HANDLE **Buffer
  )
{
  EFI_STATUS  Status = mLocateHandleBuffer (SearchType, Protocol, SearchKey, NoHandles, Buffer);
  TRACE ("LocateHandleBuffer", Status, "type=%d %g count=%lu", SearchType, Protocol,
         (UINT64)(EFI_ERROR (Status) ? 0 : *NoHandles));
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceLocateDevicePath (
  IN EFI_GUID *Protocol, IN OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath, OUT EFI_HANDLE *Device
  )
{
  EFI_STATUS  Status = mLocateDevicePath (Protocol, DevicePath, Device);
  TRACE ("LocateDevicePath", Status, "%g", Protocol);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceCreateEvent (
  IN UINT32 Type, IN EFI_TPL NotifyTpl, IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
  IN VOID *NotifyContext OPTIONAL, OUT EFI_EVENT *Event
  )
{
  EFI_STATUS  Status = mCreateEvent (Type, NotifyTpl, NotifyFunction, NotifyContext, Event);
  TRACE ("CreateEvent", Status, "type=0x%x tpl=%lu", Type, (UINT64)NotifyTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceSetTimer (
  IN EFI_EVENT Event, IN EFI_TIMER_DELAY Type, IN UINT64 TriggerTime
  )
{
  EFI_STATUS  Status = mSetTimer (Event, Type, TriggerTime);
  TRACE ("SetTimer", Status, "type=%d time=%llu", Type, TriggerTime);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceWaitForEvent (
  IN UINTN NumberOfEvents, IN EFI_EVENT *Event, OUT UINTN *Index
  )
{
  EFI_STATUS  Status = mWaitForEvent (NumberOfEvents, Event, Index);
  TRACE ("WaitForEvent", Status, "count=%lu", (UINT64)NumberOfEvents);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceSetWatchdogTimer (
  IN UINTN Timeout, IN UINT64 WatchdogCode, IN UINTN DataSize, IN CHAR16 *WatchdogData OPTIONAL
  )
{
  EFI_STATUS  Status = mSetWatchdogTimer (Timeout, WatchdogCode, DataSize, WatchdogData);
  TRACE ("SetWatchdogTimer", Status, "timeout=%lu", (UINT64)Timeout);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceLoadImage (
  IN BOOLEAN BootPolicy, IN EFI_HANDLE ParentImageHandle,
  IN EFI_DEVICE_PATH_PROTOCOL *DevicePath, IN VOID *SourceBuffer OPTIONAL,
  IN UINTN SourceSize, OUT EFI_HANDLE *ImageHandle
  )
{
  EFI_STATUS  Status = mLoadImage (BootPolicy, ParentImageHandle, DevicePath, SourceBuffer,
                                   SourceSize, ImageHandle);
  TRACE ("LoadImage", Status, "policy=%d srcsize=0x%lx", BootPolicy, (UINT64)SourceSize);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceStartImage (
  IN EFI_HANDLE ImageHandle, OUT UINTN *ExitDataSize, OUT CHAR16 **ExitData OPTIONAL
  )
{
  EFI_STATUS  Status = mStartImage (ImageHandle, ExitDataSize, ExitData);
  TRACE ("StartImage", Status, "handle=%p", ImageHandle);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceInstallConfigurationTable (
  IN EFI_GUID *Guid, IN VOID *Table
  )
{
  EFI_STATUS  Status = mInstallConfigurationTable (Guid, Table);
  TRACE ("InstallConfigurationTable", Status, "%g table=%p", Guid, Table);
  return Status;
}


STATIC
EFI_STATUS
EFIAPI
TraceExit (
  IN EFI_HANDLE ImageHandle, IN EFI_STATUS ExitStatus, IN UINTN ExitDataSize,
  IN CHAR16 *ExitData OPTIONAL
  )
{
  //
  // This is how a UEFI application returns a status, so it is the call that finally
  // reports bootmgfw's verdict - and it never comes back.
  //
  DEBUG ((DEBUG_ERROR, "BS: Exit handle=%p status=%r datasize=%lu\n",
          ImageHandle, ExitStatus, (UINT64)ExitDataSize));
  if ((ExitData != NULL) && (ExitDataSize >= sizeof (CHAR16))) {
    DEBUG ((DEBUG_ERROR, "BS: Exit data: %s\n", ExitData));
  }

  return mExit (ImageHandle, ExitStatus, ExitDataSize, ExitData);
}

STATIC
EFI_STATUS
EFIAPI
TraceFreePool (
  IN VOID *Buffer
  )
{
  EFI_STATUS  Status = mFreePool (Buffer);
  TRACE ("FreePool", Status, "buf=%p", Buffer);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceLocateHandle (
  IN EFI_LOCATE_SEARCH_TYPE SearchType, IN EFI_GUID *Protocol OPTIONAL,
  IN VOID *SearchKey OPTIONAL, IN OUT UINTN *BufferSize, OUT EFI_HANDLE *Buffer
  )
{
  EFI_STATUS  Status = mLocateHandle (SearchType, Protocol, SearchKey, BufferSize, Buffer);
  TRACE ("LocateHandle", Status, "type=%d %g", SearchType, Protocol);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceStall (
  IN UINTN Microseconds
  )
{
  EFI_STATUS  Status = mStall (Microseconds);
  TRACE ("Stall", Status, "us=%lu", (UINT64)Microseconds);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceCloseEvent (
  IN EFI_EVENT Event
  )
{
  EFI_STATUS  Status = mCloseEvent (Event);
  TRACE ("CloseEvent", Status, "event=%p", Event);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceSignalEvent (
  IN EFI_EVENT Event
  )
{
  EFI_STATUS  Status = mSignalEvent (Event);
  TRACE ("SignalEvent", Status, "event=%p", Event);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceCheckEvent (
  IN EFI_EVENT Event
  )
{
  EFI_STATUS  Status = mCheckEvent (Event);
  TRACE ("CheckEvent", Status, "event=%p", Event);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceCreateEventEx (
  IN UINT32 Type, IN EFI_TPL NotifyTpl, IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
  IN CONST VOID *NotifyContext OPTIONAL, IN CONST EFI_GUID *EventGroup OPTIONAL,
  OUT EFI_EVENT *Event
  )
{
  EFI_STATUS  Status = mCreateEventEx (Type, NotifyTpl, NotifyFunction, NotifyContext,
                                       EventGroup, Event);
  TRACE ("CreateEventEx", Status, "type=0x%x group=%g", Type, EventGroup);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceCloseProtocol (
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, IN EFI_HANDLE AgentHandle,
  IN EFI_HANDLE ControllerHandle
  )
{
  EFI_STATUS  Status = mCloseProtocol (Handle, Protocol, AgentHandle, ControllerHandle);
  TRACE ("CloseProtocol", Status, "handle=%p %g", Handle, Protocol);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceRegisterProtocolNotify (
  IN EFI_GUID *Protocol, IN EFI_EVENT Event, OUT VOID **Registration
  )
{
  EFI_STATUS  Status = mRegisterProtocolNotify (Protocol, Event, Registration);
  TRACE ("RegisterProtocolNotify", Status, "%g", Protocol);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceInstallProtocolInterface (
  IN OUT EFI_HANDLE *Handle, IN EFI_GUID *Protocol, IN EFI_INTERFACE_TYPE InterfaceType,
  IN VOID *Interface
  )
{
  EFI_STATUS  Status = mInstallProtocolInterface (Handle, Protocol, InterfaceType, Interface);
  TRACE ("InstallProtocolInterface", Status, "%g", Protocol);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceConnectController (
  IN EFI_HANDLE ControllerHandle, IN EFI_HANDLE *DriverImageHandle OPTIONAL,
  IN EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath OPTIONAL, IN BOOLEAN Recursive
  )
{
  EFI_STATUS  Status = mConnectController (ControllerHandle, DriverImageHandle,
                                           RemainingDevicePath, Recursive);
  TRACE ("ConnectController", Status, "handle=%p", ControllerHandle);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceExitBootServices (
  IN EFI_HANDLE ImageHandle, IN UINTN MapKey
  )
{
  DEBUG ((DEBUG_ERROR, "BS: ExitBootServices handle=%p mapkey=0x%lx\n",
          ImageHandle, (UINT64)MapKey));
  return mExitBootServices (ImageHandle, MapKey);
}


STATIC
EFI_STATUS
EFIAPI
TraceGetVariable (
  IN CHAR16 *VariableName, IN EFI_GUID *VendorGuid, OUT UINT32 *Attributes OPTIONAL,
  IN OUT UINTN *DataSize, OUT VOID *Data OPTIONAL
  )
{
  EFI_STATUS  Status = mGetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  TRACE ("RT:GetVariable", Status, "%s %g size=%lu", VariableName, VendorGuid, (UINT64)*DataSize);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceSetVariable (
  IN CHAR16 *VariableName, IN EFI_GUID *VendorGuid, IN UINT32 Attributes,
  IN UINTN DataSize, IN VOID *Data
  )
{
  EFI_STATUS  Status = mSetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  TRACE ("RT:SetVariable", Status, "%s %g size=%lu", VariableName, VendorGuid, (UINT64)DataSize);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceGetNextVariableName (
  IN OUT UINTN *NameSize, IN OUT CHAR16 *VariableName, IN OUT EFI_GUID *VendorGuid
  )
{
  EFI_STATUS  Status = mGetNextVariableName (NameSize, VariableName, VendorGuid);
  TRACE ("RT:GetNextVariableName", Status, "size=%lu", (UINT64)*NameSize);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceQueryVariableInfo (
  IN UINT32 Attributes, OUT UINT64 *MaxStorage, OUT UINT64 *RemainStorage, OUT UINT64 *MaxSize
  )
{
  EFI_STATUS  Status = mQueryVariableInfo (Attributes, MaxStorage, RemainStorage, MaxSize);
  TRACE ("RT:QueryVariableInfo", Status, "attr=0x%x", Attributes);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceGetTime (
  OUT EFI_TIME *Time, OUT EFI_TIME_CAPABILITIES *Capabilities OPTIONAL
  )
{
  EFI_STATUS  Status = mGetTime (Time, Capabilities);
  if (!EFI_ERROR (Status)) {
    TRACE ("RT:GetTime", Status, "%04u-%02u-%02u %02u:%02u:%02u",
           Time->Year, Time->Month, Time->Day, Time->Hour, Time->Minute, Time->Second);
  } else {
    TRACE ("RT:GetTime", Status, "%a", "");
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceSetTime (
  IN EFI_TIME *Time
  )
{
  EFI_STATUS  Status = mSetTime (Time);
  TRACE ("RT:SetTime", Status, "%a", "");
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TraceGetWakeupTime (
  OUT BOOLEAN *Enabled, OUT BOOLEAN *Pending, OUT EFI_TIME *Time
  )
{
  EFI_STATUS  Status = mGetWakeupTime (Enabled, Pending, Time);
  TRACE ("RT:GetWakeupTime", Status, "%a", "");
  return Status;
}

STATIC
VOID
FixCrc (
  VOID
  )
{
  gBS->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  gRT->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gRT, gRT->Hdr.HeaderSize, &gRT->Hdr.CRC32);
}

#define HOOK(Field, Fn)  do { m##Field = gBS->Field; gBS->Field = Fn; } while (FALSE)
#define UNHOOK(Field)    do { gBS->Field = m##Field; } while (FALSE)

STATIC
VOID
InstallHooks (
  VOID
  )
{
  HOOK (AllocatePages, TraceAllocatePages);
  HOOK (FreePages, TraceFreePages);
  HOOK (GetMemoryMap, TraceGetMemoryMap);
  HOOK (AllocatePool, TraceAllocatePool);
  HOOK (HandleProtocol, TraceHandleProtocol);
  HOOK (LocateProtocol, TraceLocateProtocol);
  HOOK (OpenProtocol, TraceOpenProtocol);
  HOOK (LocateHandleBuffer, TraceLocateHandleBuffer);
  HOOK (LocateDevicePath, TraceLocateDevicePath);
  HOOK (CreateEvent, TraceCreateEvent);
  HOOK (SetTimer, TraceSetTimer);
  HOOK (WaitForEvent, TraceWaitForEvent);
  HOOK (SetWatchdogTimer, TraceSetWatchdogTimer);
  HOOK (LoadImage, TraceLoadImage);
  HOOK (StartImage, TraceStartImage);
  HOOK (InstallConfigurationTable, TraceInstallConfigurationTable);
  HOOK (Exit, TraceExit);
  HOOK (FreePool, TraceFreePool);
  HOOK (LocateHandle, TraceLocateHandle);
  HOOK (Stall, TraceStall);
  HOOK (CloseEvent, TraceCloseEvent);
  HOOK (SignalEvent, TraceSignalEvent);
  HOOK (CheckEvent, TraceCheckEvent);
  HOOK (CreateEventEx, TraceCreateEventEx);
  HOOK (CloseProtocol, TraceCloseProtocol);
  HOOK (RegisterProtocolNotify, TraceRegisterProtocolNotify);
  HOOK (InstallProtocolInterface, TraceInstallProtocolInterface);
  HOOK (ConnectController, TraceConnectController);
  HOOK (ExitBootServices, TraceExitBootServices);

  mGetVariable         = gRT->GetVariable;         gRT->GetVariable         = TraceGetVariable;
  mSetVariable         = gRT->SetVariable;         gRT->SetVariable         = TraceSetVariable;
  mGetNextVariableName = gRT->GetNextVariableName; gRT->GetNextVariableName = TraceGetNextVariableName;
  mQueryVariableInfo   = gRT->QueryVariableInfo;   gRT->QueryVariableInfo   = TraceQueryVariableInfo;
  mGetTime             = gRT->GetTime;             gRT->GetTime             = TraceGetTime;
  mSetTime             = gRT->SetTime;             gRT->SetTime             = TraceSetTime;
  mGetWakeupTime       = gRT->GetWakeupTime;       gRT->GetWakeupTime       = TraceGetWakeupTime;
  FixCrc ();
}

STATIC
VOID
RemoveHooks (
  VOID
  )
{
  UNHOOK (AllocatePages);
  UNHOOK (FreePages);
  UNHOOK (GetMemoryMap);
  UNHOOK (AllocatePool);
  UNHOOK (HandleProtocol);
  UNHOOK (LocateProtocol);
  UNHOOK (OpenProtocol);
  UNHOOK (LocateHandleBuffer);
  UNHOOK (LocateDevicePath);
  UNHOOK (CreateEvent);
  UNHOOK (SetTimer);
  UNHOOK (WaitForEvent);
  UNHOOK (SetWatchdogTimer);
  UNHOOK (LoadImage);
  UNHOOK (StartImage);
  UNHOOK (InstallConfigurationTable);
  UNHOOK (Exit);
  UNHOOK (FreePool);
  UNHOOK (LocateHandle);
  UNHOOK (Stall);
  UNHOOK (CloseEvent);
  UNHOOK (SignalEvent);
  UNHOOK (CheckEvent);
  UNHOOK (CreateEventEx);
  UNHOOK (CloseProtocol);
  UNHOOK (RegisterProtocolNotify);
  UNHOOK (InstallProtocolInterface);
  UNHOOK (ConnectController);
  UNHOOK (ExitBootServices);

  gRT->GetVariable         = mGetVariable;
  gRT->SetVariable         = mSetVariable;
  gRT->GetNextVariableName = mGetNextVariableName;
  gRT->QueryVariableInfo   = mQueryVariableInfo;
  gRT->GetTime             = mGetTime;
  gRT->SetTime             = mSetTime;
  gRT->GetWakeupTime       = mGetWakeupTime;
  FixCrc ();
}

STATIC
EFI_DEVICE_PATH_PROTOCOL *
BuildPathFromShellArg (
  IN CHAR16  *Arg
  )
{
  //
  // The argument arrives as a shell path ("FS2:\EFI\BOOT\BOOTAA64.EFI"). Resolving a shell
  // mapping properly needs the shell protocol; rather than depend on it, take the part from
  // the first backslash and try it on every filesystem handle. There is one real volume on
  // this bench, so this is unambiguous in practice.
  //
  EFI_STATUS  Status;
  UINTN       Count;
  EFI_HANDLE  *Handles;
  UINTN       Index;
  CHAR16      *FileName;

  FileName = Arg;
  while ((*FileName != L'\0') && (*FileName != L'\\')) {
    FileName++;
  }

  if (*FileName == L'\0') {
    Print (L"The path must contain '\\', for example FS2:\\EFI\\BOOT\\BOOTAA64.EFI\n");
    return NULL;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"No filesystem is available: %r\n", Status);
    return NULL;
  }

  for (Index = 0; Index < Count; Index++) {
    EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
    EFI_DEVICE_PATH_PROTOCOL  *Remaining;
    EFI_HANDLE                TestHandle;

    DevicePath = FileDevicePath (Handles[Index], FileName);
    if (DevicePath == NULL) {
      continue;
    }

    Remaining = DevicePath;
    Status    = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Remaining, &TestHandle);
    if (!EFI_ERROR (Status) && (TestHandle == Handles[Index])) {
      FreePool (Handles);
      return DevicePath;
    }

    FreePool (DevicePath);
  }

  FreePool (Handles);
  Print (L"No volume contains %s\n", FileName);
  return NULL;
}

EFI_STATUS
EFIAPI
BootLaunchAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                     Status;
  EFI_SHELL_PARAMETERS_PROTOCOL  *Params;
  EFI_DEVICE_PATH_PROTOCOL       *DevicePath;
  EFI_HANDLE                     NewImage;
  EFI_LOADED_IMAGE_PROTOCOL      *LoadedImage;
  UINTN                          ExitDataSize;
  CHAR16                         *ExitData;
  UINTN                          Index;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiShellParametersProtocolGuid, (VOID **)&Params);
  if (EFI_ERROR (Status) || (Params->Argc < 2)) {
    Print (L"Usage: BootLaunchApp.efi <path-to-.efi> [-q]\n");
    Print (L"  -q  show failed calls only\n");
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 2; Index < Params->Argc; Index++) {
    if (StrCmp (Params->Argv[Index], L"-q") == 0) {
      mFailuresOnly = TRUE;
    }
  }

  DevicePath = BuildPathFromShellArg (Params->Argv[1]);
  if (DevicePath == NULL) {
    return EFI_NOT_FOUND;
  }

  NewImage = NULL;
  Status   = gBS->LoadImage (FALSE, ImageHandle, DevicePath, NULL, 0, &NewImage);
  DEBUG ((DEBUG_ERROR, "LAUNCH: LoadImage -> %r\n", Status));
  if (EFI_ERROR (Status)) {
    FreePool (DevicePath);
    return Status;
  }

  //
  // Hand the image no options at all, the way BDS would for a boot option with no
  // OptionalData.
  //
  Status = gBS->HandleProtocol (NewImage, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "LAUNCH: LoadOptions was %u bytes, clearing\n", LoadedImage->LoadOptionsSize));
    LoadedImage->LoadOptions     = NULL;
    LoadedImage->LoadOptionsSize = 0;
  }

  ExitData     = NULL;
  ExitDataSize = 0;

  DEBUG ((DEBUG_ERROR, "=== BootLaunchApp: tracing enabled ===\n"));
  InstallHooks ();
  Status = mStartImage (NewImage, &ExitDataSize, &ExitData);
  RemoveHooks ();
  DEBUG ((DEBUG_ERROR, "=== BootLaunchApp: StartImage -> %r ===\n", Status));

  DEBUG ((DEBUG_ERROR, "LAUNCH: StartImage -> %r\n", Status));

  if ((ExitData != NULL) && (ExitDataSize >= sizeof (CHAR16))) {
    DEBUG ((DEBUG_ERROR, "LAUNCH: ExitData: %s\n", ExitData));
    FreePool (ExitData);
  }

  FreePool (DevicePath);
  return Status;
}

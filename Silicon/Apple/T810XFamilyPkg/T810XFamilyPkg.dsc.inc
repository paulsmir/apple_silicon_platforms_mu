[PcdsPatchableInModule.common]
  #These will get overriden by the FDT settings
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x800000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x200000000 #8GB RAM space by default
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x0
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x0
  gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferAddress|0x0
  gAppleSiliconPkgTokenSpaceGuid.PcdFrameBufferSize|0x0

[PcdsFixedAtBuild.common]
  # Setting timer frequency to 0 means "read from CNTFRQ_EL0"
  #gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|0
  #gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x8FF000000 #fair warning this location is *totally random*
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|19 # placeholder 
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|17 # phys timer IRQ number (this is arbitrary because per-cpu fiq)
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|20 #this disables the HV timer
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|18 #virt timer
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleSartVersion|2
  # EXPERIMENT: upstream hardcodes these to 0x840000000/0x840004000. Under the m1n1
  # hypervisor that address is unusable in both layouts we tried: with a pinned
  # phys_base of 0x850000000 it is below guest RAM ("HV: Unmapped IPA 0x840000000"),
  # and with m1n1's natural layout it lands in a carveout, where the access faults
  # inside m1n1's own EL2 handler and reboots the machine.
  # 0x854000000 sits inside pinned guest RAM, ~12 MB above the end of the loaded
  # image region (which ends around 0x8533ec000). Keep in sync with run_uefi.py.
  gAppleSiliconPkgTokenSpaceGuid.PcdBootArgsPointer|0x854000000
  # m1n1's hv uploads the ADT to adt_base = phys_base + 16 MB (hv/__init__.py, start()).
  # With run_uefi.py pinning phys_base to 0x850000000 that is exactly 0x851000000.
  # The old 0x854004000 pointed 48 MB past it, so dt_get() parsed garbage and every
  # ADT-driven driver (AppleUsbTypeCBringupDxe in particular) silently found nothing.
  gAppleSiliconPkgTokenSpaceGuid.PcdAdtPointer|0x851000000

  # Window for a RAMDisk preloaded straight into guest RAM by run_uefi.py. Sits well above
  # the firmware volume (0x8510B4000 + 0x1E00000) and inside the RAM m1n1 maps for the
  # guest, and is carved out of the memory map by MemoryInitPeiLib.
  gAppleSiliconPkgTokenSpaceGuid.PcdPreloadedRamdiskBase|0x860000000
  gAppleSiliconPkgTokenSpaceGuid.PcdPreloadedRamdiskMaxSize|0x40000000

  # Low DRAM window for the Windows boot manager. Starts at 1 MB rather than 0 so that a
  # null dereference in the firmware still faults. Backed by RAM above the RAMDisk window;
  # guest DRAM runs to 0x9DF708000, so 0x8A0100000 + 0x3FF00000 fits with room to spare.
  gAppleSiliconPkgTokenSpaceGuid.PcdLowMemoryWindowBase|0x100000
  gAppleSiliconPkgTokenSpaceGuid.PcdLowMemoryWindowSize|0x3FF00000
  gAppleSiliconPkgTokenSpaceGuid.PcdLowMemoryWindowBackingBase|0x8A0100000
!if $(USES_MAC_CPU) == TRUE
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosCpuIdentifier|"T8103"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosCpuModel|"Apple M1"
!else
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosCpuIdentifier|"T8101"
  gAppleSiliconPkgTokenSpaceGuid.PcdSmbiosCpuModel|"Apple A14-compatible CPU"
!endif
  #Serial port base addr
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleUartBase|0x235200000
  gAppleSiliconPkgTokenSpaceGuid.PcdAppleSocIdentifier|0x8103
  gAppleSiliconPkgTokenSpaceGuid.PcdInitializeRamdisk|TRUE
  gAppleSiliconPkgTokenSpaceGuid.PcdSecPhaseStackBase|0x900000000
  #number of cores (right now we are assuming the lowest common denominator - this will change later)
  gAppleSiliconPkgTokenSpaceGuid.PcdCoreCount|8|UINT32|0x00004406
  #all Rhodes chips have three clusters, "boot", P-core/E-core clusters
  # for multi-die systems, this cluster setup is duplicated on the second die
  gAppleSiliconPkgTokenSpaceGuid.PcdClusterCount|2|UINT32|0x00004407

  #number of cores (right now we are assuming the lowest common denominator - this will change later)
  gAppleSiliconPkgTokenSpaceGuid.PcdCoreCount|8|UINT32|0x00004406
  #all Rhodes chips have three clusters, "boot", P-core/E-core clusters
  # for multi-die systems, this cluster setup is duplicated on the second die
  gAppleSiliconPkgTokenSpaceGuid.PcdClusterCount|2|UINT32|0x00004407

#AIC/GIC configuration settings
!if $(AIC_BUILD) == FALSE
  #
  # if GIC is used, it's expected to be virtualized by trusted firmware at the following addresses.
  # as Tonga/T8103 and Sicily/T8101 has no onboard GIC. (but do contain the MSRs necessary for accelerated virt)
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0xf00000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0xf10000000
  #
  # EXPERIMENT: the GICv2 CPU interface base was missing, so it defaulted to 0 and
  # ArmGicV2Dxe faulted writing GICC_BPR at address 0x8 (observed: "str w1, [x0]"
  # with x0 = 8). It is needed because ArmGicDxe's GicV3Supported() decides from
  # ArmHasGicSystemRegisters(), and the M1 has no GIC and hence no ICC_* system
  # registers, so EDK2 always drives the GIC as a v2 over MMIO.
  # Must match CPU_IFACE_BASE_36_BIT in m1n1_windows/src/hv_vgic.c.
  #
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0xf30000000
!endif
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00008103

!if $(SECURE_BOOT_ENABLE) == TRUE
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|600
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|400
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesCode|1500
!else
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|300
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|150
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesCode|1000
!endif
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesData|800
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderCode|10
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderData|0
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|40
  
  #PCIe settings
  gEfiMdeModulePkgTokenSpaceGuid.PcdSrIovSupport|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdAriSupport|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdMrIovSupport|FALSE
  #placeholder settings
  gArmTokenSpaceGuid.PcdPciBusMin|0x0
  gArmTokenSpaceGuid.PcdPciBusMax|0xFF
  #
  # Apple chips don't seem to use "x86-style" I/O accesses, so use placeholder values here, and don't reference them in the host bridge driver.
  #
  gArmTokenSpaceGuid.PcdPciIoBase|0x0
  gArmTokenSpaceGuid.PcdPciIoSize|0x00010000
  #
  # The 32 bit PCIe MMIO is not prefetchable.
  #
  gArmTokenSpaceGuid.PcdPciMmio32Base|0xc0000000
  gArmTokenSpaceGuid.PcdPciMmio32Size|0x40000000
  gEfiMdePkgTokenSpaceGuid.PcdPciMmio32Translation|0x500000000
  gEfiMdePkgTokenSpaceGuid.PcdPciMmio64Translation|0x0
  #
  # 64 bit PCIe MMIO *is* prefetchable.
  #
  gArmTokenSpaceGuid.PcdPciMmio64Base|0x400000000
  gArmTokenSpaceGuid.PcdPciMmio64Size|0x20000000

  gEfiMdePkgTokenSpaceGuid.PcdPciExpressBaseAddress|0x690000000
  gAppleSiliconPkgTokenSpaceGuid.PcdPciExpressBarSize|0x1000000
  gAppleSiliconPkgTokenSpaceGuid.PcdPciExpressBarLimit|0x690FFFFFF

  gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0xFFFFFFFB00000000
  #
  # This shouldn't get used.
  #
  gEfiMdePkgTokenSpaceGuid.PcdPciIoTranslation|0x0

  #
  # Apple chips don't seem to use "x86-style" I/O accesses, so use placeholder values here, and don't reference them in the host bridge driver.
  #
  gArmTokenSpaceGuid.PcdPciIoBase|0x0
  gArmTokenSpaceGuid.PcdPciIoSize|0x00010000
  #
  # The 32 bit PCIe MMIO is not prefetchable.
  #
  gArmTokenSpaceGuid.PcdPciMmio32Base|0xc0000000
  gArmTokenSpaceGuid.PcdPciMmio32Size|0x40000000
  gEfiMdePkgTokenSpaceGuid.PcdPciMmio32Translation|0x500000000
  gEfiMdePkgTokenSpaceGuid.PcdPciMmio64Translation|0x0
  #
  # 64 bit PCIe MMIO *is* prefetchable.
  #
  gArmTokenSpaceGuid.PcdPciMmio64Base|0x400000000
  gArmTokenSpaceGuid.PcdPciMmio64Size|0x20000000

  gEfiMdePkgTokenSpaceGuid.PcdPciExpressBaseAddress|0x690000000
  gAppleSiliconPkgTokenSpaceGuid.PcdPciExpressBarSize|0x1000000
  gAppleSiliconPkgTokenSpaceGuid.PcdPciExpressBarLimit|0x690FFFFFF

  gEmbeddedTokenSpaceGuid.PcdDmaDeviceOffset|0xFFFFFFFB00000000
  #
  # This shouldn't get used.
  #
  gEfiMdePkgTokenSpaceGuid.PcdPciIoTranslation|0x0

[LibraryClasses.common]
  MemoryInitPeiLib|T810XFamilyPkg/Library/MemoryInitPeiLib/MemoryInitPeiLib.inf
  MsPlatformDevicesLib  |T810XFamilyPkg/Library/MsPlatformDevicesLib/MsPlatformDevicesLib.inf

[Components.common]

  T810XFamilyPkg/AcpiTables/CPUAcpiTables.inf
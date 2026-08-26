/**
 * Opt-in J313 Apple AGX G2 enumeration table.
 *
 * This table only declares reviewed resources.  It does not start AGX, change
 * clocks, write registers, or claim firmware ownership.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 **/
#include <IndustryStandard/Acpi65.h>

DefinitionBlock ("", "SSDT", 0x02, "Apple", "J313AGX", 0x00000001)
{
    Scope (\_SB)
    {
        Include ("J313AppleAgx.asl.inc")
    }
}

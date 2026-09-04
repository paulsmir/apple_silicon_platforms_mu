/**
 * Candidate-only J313 Full Graphics ABI admission table.
 *
 * Physical AGX sources remain masked.  The only interrupt visible to Windows
 * is the m1n1-owned synthetic scanout edge described by the generated include.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT
 **/
#include <IndustryStandard/Acpi65.h>

DefinitionBlock ("", "SSDT", 0x02, "Apple", "J313ABI", 0x00000001)
{
    Scope (\_SB)
    {
        Include ("J313AppleAgxAbiAdmission.asl.inc")
    }
}

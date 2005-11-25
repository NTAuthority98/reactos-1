/*++ NDK Version: 0095

Copyright (c) Alex Ionescu.  All rights reserved.

Header Name:

    inbvfuncs.h

Abstract:

    Function definitions for the Boot Video Driver.

Author:

    Alex Ionescu (alex.ionescu@reactos.com)   06-Oct-2004

--*/

#ifndef _INBVFUNCS_H
#define _INBVFUNCS_H

//
// Dependencies
//
#include <umtypes.h>

#ifndef NTOS_MODE_USER
//
// Ownership Functions
//
VOID
NTAPI
InbvAcquireDisplayOwnership(VOID);

BOOLEAN
NTAPI
InbvCheckDisplayOwnership(VOID);

VOID
NTAPI
InbvNotifyDisplayOwnershipLost(
    IN PVOID Callback
);

//
// Installation Functions
//
VOID
NTAPI
InbvEnableBootDriver(
    IN BOOLEAN Enable
);

VOID
NTAPI
InbvInstallDisplayStringFilter(
    IN PVOID Unknown
);

BOOLEAN
NTAPI
InbvIsBootDriverInstalled(VOID);

//
// Display Functions
//
BOOLEAN
NTAPI
InbvDisplayString(
    IN PCHAR String
);

BOOLEAN
NTAPI
InbvEnableDisplayString(
    IN BOOLEAN Enable
);

BOOLEAN
NTAPI
InbvResetDisplay(VOID);

VOID
NTAPI
InbvSetScrollRegion(
    IN ULONG Left,
    IN ULONG Top,
    IN ULONG Width,
    IN ULONG Height
);

VOID
NTAPI
InbvSetTextColor(
    IN ULONG Color
);

VOID
NTAPI
InbvSolidColorFill(
    IN ULONG Left,
    IN ULONG Top,
    IN ULONG Width,
    IN ULONG Height,
    IN ULONG Color
);

#endif
#endif

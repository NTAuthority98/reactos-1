/*
 * fnmatch.h
 *
 * filename-matching types. Based on the Single UNIX(r) Specification,
 * Version 2
 *
 * This file is part of the ReactOS Operating System.
 *
 * Contributors:
 *  Created by KJK::Hyperion <noog@libero.it>
 *
 *  THIS SOFTWARE IS NOT COPYRIGHTED
 *
 *  This source code is offered for use in the public domain. You may
 *  use, modify or distribute it freely.
 *
 *  This code is distributed in the hope that it will be useful but
 *  WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 *  DISCLAMED. This includes but is not limited to warranties of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 */
#ifndef __FNMATCH_H_INCLUDED__
#define __FNMATCH_H_INCLUDED__

/* INCLUDES */
#ifdef __PSXDLL__

/* headers for internal usage by psxdll.dll and ReactOS */

#else /* ! __PSXDLL__ */

/* standard POSIX headers */

#endif

/* OBJECTS */

/* TYPES */

/* CONSTANTS */
/* Flags */
#define FNM_PATHNAME (0x00000001) /* Slash in string only matches slash
                                     in pattern. */
#define FNM_PERIOD   (0x00000002) /* Leading period in string must be
                                     exactly matched by period in
                                     pattern. */
#define FNM_NOESCAPE (0x00000004) /* Disable backslash escaping. */

/* Return values */
#define FNM_NOMATCH (1) /* The string does not match the specified
                           pattern. */
#define FNM_NOSYS   (2) /* The implementation does not support this
                           function. */

/* PROTOTYPES */
int fnmatch(const char *, const char *, int);

/* MACROS */

#endif /* __FNMATCH_H_INCLUDED__ */

/* EOF */


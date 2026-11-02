// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

/* The one domain account wbclient_stub.c answers for, shared with the wire
 * probe that logs on as it.  The user and password match the account
 * scripts/ad_test_wrapper.sh creates, so the probe's winbind profiles run
 * unchanged against either. */
#define WBSTUB_DOMAIN       "WBSTUB"
#define WBSTUB_DNS_DOMAIN   "wbstub.test"
#define WBSTUB_NETBIOS_NAME "WBSTUBSRV"
#define WBSTUB_USER         "testuser1"
#define WBSTUB_PASSWORD     "Password1!"

/* Distinct from the wire probe's local account (uid/gid 1000), so a logon that
* fell through to a local identity shows in the server log, and from each
* other, so the server's gid = uid fallback for an unmapped group shows too. */
#define WBSTUB_UID          20001u
#define WBSTUB_GID          20513u
#define WBSTUB_USER_SID     "S-1-5-21-1-2-3-1001"
#define WBSTUB_GROUP_SID    "S-1-5-21-1-2-3-513"

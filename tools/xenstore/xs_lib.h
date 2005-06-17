/* 
    Common routines between Xen store user library and daemon.
    Copyright (C) 2005 Rusty Russell IBM Corporation

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef _XS_LIB_H
#define _XS_LIB_H

#include <stdbool.h>
#include <limits.h>
#include <xc.h>

/* Bitmask of permissions. */
enum xs_perm_type {
	XS_PERM_NONE = 0,
	XS_PERM_READ = 1,
	XS_PERM_WRITE = 2,
	/* Internal use. */
	XS_PERM_CREATE = 4,
	XS_PERM_OWNER = 8,
};

struct xs_permissions
{
	domid_t id;
	enum xs_perm_type perms;
};

/* Each 10 bits takes ~ 3 digits, plus one, plus one for nul terminator. */
#define MAX_STRLEN(x) ((sizeof(x) * CHAR_BIT + CHAR_BIT-1) / 10 * 3 + 2)

/* Path for various daemon things: env vars can override. */
const char *xs_daemon_socket(void);
const char *xs_daemon_socket_ro(void);
const char *xs_daemon_store(void);
const char *xs_daemon_transactions(void);

/* Simple write function: loops for you. */
bool xs_write_all(int fd, const void *data, unsigned int len);

/* Convert strings to permissions.  False if a problem. */
bool xs_strings_to_perms(struct xs_permissions *perms, unsigned int num,
			 const char *strings);

/* Convert permissions to a string (up to len MAX_STRLEN(domid_t)+1). */
bool xs_perm_to_string(const struct xs_permissions *perm, char *buffer);

/* Given a string and a length, count how many strings (nul terms). */
unsigned int xs_count_strings(const char *strings, unsigned int len);

#endif /* _XS_LIB_H */

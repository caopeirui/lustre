/*
 *    This Cplant(TM) source code is the property of Sandia National
 *    Laboratories.
 *
 *    This Cplant(TM) source code is copyrighted by Sandia National
 *    Laboratories.
 *
 *    The redistribution of this Cplant(TM) source code is subject to the
 *    terms of the GNU Lesser General Public License
 *    (see cit/LGPL or http://www.gnu.org/licenses/lgpl.html)
 *
 *    Cplant(TM) Copyright 1998-2003 Sandia Corporation. 
 *    Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 *    license for use of this work by or on behalf of the US Government.
 *    Export of this program may require a license from the United States
 *    Government.
 */

/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Questions or comments about this library should be sent to:
 *
 * Lee Ward
 * Sandia National Laboratories, New Mexico
 * P.O. Box 5800
 * Albuquerque, NM 87185-1110
 *
 * lee@sandia.gov
 */

#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>

#include "sysio.h"
#include "inode.h"
#include "file.h"

#include "sysio-symbols.h"

static _SYSIO_OFF_T
_sysio_lseek(int fd, _SYSIO_OFF_T offset, int whence)
{
	struct file *fil;
	_SYSIO_OFF_T off, pos;
	struct intnl_stat stbuf;

	fil = _sysio_fd_find(fd);
	if (!fil)
		return -EBADF;

	off = -1;
	switch (whence) {
	
	case SEEK_SET:
		off = 0;
		break;
	case SEEK_CUR:
		off = fil->f_pos;
		break;
	case SEEK_END:
		{
			int	err;

			err =
			    (*fil->f_ino->i_ops.inop_getattr)(NULL,
							      fil->f_ino,
							      &stbuf);
			if (err)
				return err;
	
		}
		off = stbuf.st_size;
		break;
	default:
		return -EINVAL;
	}
	pos = off + offset;
	if ((offset < 0 && -offset > off) || (offset > 0 && pos <= off))
		return -EINVAL;

#ifdef O_LARGEFILE
	if (pos >= ((fil->f_flags & O_LARGEFILE) ? _SYSIO_OFF_T_MAX : LONG_MAX))
		return -EOVERFLOW;
#else
	if (pos >= _SYSIO_OFF_T_MAX)
		return -EOVERFLOW;
#endif
	pos = (fil->f_ino->i_ops.inop_pos)(fil->f_ino, pos);
	if (pos < 0)
		return pos;
	fil->f_pos = pos;
	return pos;
}

#if _LARGEFILE64_SOURCE
#undef lseek64

extern off64_t
SYSIO_INTERFACE_NAME(lseek64)(int fd, off64_t offset, int whence)
{
	_SYSIO_OFF_T off;
	off_t	rtn;
	SYSIO_INTERFACE_DISPLAY_BLOCK;

	SYSIO_INTERFACE_ENTER;
	off = _sysio_lseek(fd, offset, whence);
	if (off < 0)
		SYSIO_INTERFACE_RETURN((off_t )-1, (int )off);
	rtn = (off64_t )off;
	assert(rtn == off);
	SYSIO_INTERFACE_RETURN(rtn, 0);
}
#ifdef __GLIBC__
#undef __lseek64
sysio_sym_weak_alias(SYSIO_INTERFACE_NAME(lseek64),
		     PREPEND(__, SYSIO_INTERFACE_NAME(lseek64)))
#endif
#ifdef REDSTORM
#undef __libc_lseek64
sysio_sym_weak_alias(SYSIO_INTERFACE_NAME(lseek64),
		     PREPEND(__, SYSIO_INTERFACE_NAME(libc_lseek64)))
#endif
#endif

#undef lseek

extern off_t
SYSIO_INTERFACE_NAME(lseek)(int fd, off_t offset, int whence)
{
	_SYSIO_OFF_T off;
	off_t	rtn;
	SYSIO_INTERFACE_DISPLAY_BLOCK;

	SYSIO_INTERFACE_ENTER;
	off = _sysio_lseek(fd, offset, whence);
	if (off < 0)
		SYSIO_INTERFACE_RETURN((off_t )-1, (int )off);
	rtn = (off_t )off;
	assert(rtn == off);
	SYSIO_INTERFACE_RETURN(rtn, 0);
}

#ifdef __GLIBC__
#undef __lseek
sysio_sym_weak_alias(SYSIO_INTERFACE_NAME(lseek),
		     PREPEND(__, SYSIO_INTERFACE_NAME(lseek)))
#endif

#if 0
#ifdef __linux__
#undef llseek
int
SYSIO_INTERFACE_NAME(llseek)(unsigned int fd __IS_UNUSED,
       unsigned long offset_high __IS_UNUSED,
       unsigned long offset_low __IS_UNUSED,
       loff_t *result __IS_UNUSED,
       unsigned int whence __IS_UNUSED)
{
	SYSIO_INTERFACE_DISPLAY_BLOCK;

	/*
	 * Something is very wrong if this was called.
	 */
	SYSIO_INTERFACE_ENTER;
	SYSIO_INTERFACE_RETURN(-1, -ENOTSUP);
}

#undef __llseek
sysio_sym_weak_alias(SYSIO_INTERFACE_NAME(llseek), 
		     PREPEND(__, SYSIO_INTERFACE_NAME(llseek)))
#endif
#endif

// POSIX asynchronous I/O for Win32
//
// Copyright (c) 2003 Jan Wassenberg
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// Contact info:
//   Jan.Wassenberg@stud.uni-karlsruhe.de
//   http://www.stud.uni-karlsruhe.de/~urkt/

#include "precompiled.h"

#include "lib.h"
#include "win_internal.h"

#include <assert.h>
#include <stdlib.h>


#define lock() win_lock(WAIO_CS)
#define unlock() win_unlock(WAIO_CS)


#pragma data_seg(".LIB$WIC")
WIN_REGISTER_FUNC(waio_init);
#pragma data_seg(".LIB$WTX")
WIN_REGISTER_FUNC(waio_shutdown);
#pragma data_seg()


//////////////////////////////////////////////////////////////////////////////
//
// associate async-capable handle with POSIX file descriptor (int)
//
//////////////////////////////////////////////////////////////////////////////

// current implementation: open both versions of the file on open()
//   wastes 1 handle/file, but we don't have to remember the filename/mode
//
// note: current Windows lowio handle limit is 2k

static HANDLE* aio_hs;
	// array; expanded when needed in aio_h_set

static int aio_hs_size;
	// often compared against fd => int


// no init needed.

static void aio_h_cleanup()
{
	lock();

	for(int i = 0; i < aio_hs_size; i++)
		if(aio_hs[i] != INVALID_HANDLE_VALUE)
		{
			if(!CloseHandle(aio_hs[i]))
				debug_warn("CloseHandle failed");
			aio_hs[i] = INVALID_HANDLE_VALUE;
		}

	free(aio_hs);
	aio_hs = 0;

	aio_hs_size = 0;

	unlock();
}


static bool is_valid_file_handle(const HANDLE h)
{
	bool valid = (GetFileSize(h, 0) != INVALID_FILE_SIZE);
	assert(valid);
	return valid;
}


// get async capable handle to file <fd>
HANDLE aio_h_get(const int fd)
{
	HANDLE h = INVALID_HANDLE_VALUE;

	lock();

	if(0 <= fd && fd < (int)aio_hs_size)
	{
		h = aio_hs[fd];
		if(!is_valid_file_handle(h))
			h = INVALID_HANDLE_VALUE;
	}
	else
		debug_warn("aio_h_get: fd's aio handle not set");
		// h already INVALID_HANDLE_VALUE

	unlock();

	return h;
}


int aio_h_set(const int fd, const HANDLE h)
{
	lock();

	const char* msg = 0;

	if(fd < 0)
		goto fail;

	// grow hs array to at least fd+1 entries
	if(fd >= aio_hs_size)
	{
		const uint size2 = (uint)round_up(fd+8, 8);
		HANDLE* const hs2 = (HANDLE*)realloc(aio_hs, size2*sizeof(HANDLE));
		if(!hs2)
			goto fail;
		// don't assign directly from realloc -
		// we'd leak the previous array if realloc fails.

		for(uint i = aio_hs_size; i < size2; i++)
			hs2[i] = INVALID_HANDLE_VALUE;
		aio_hs = hs2;
		aio_hs_size = size2;
	}

	// nothing to do; will set aio_hs[fd] to INVALID_HANDLE_VALUE below.
	if(h == INVALID_HANDLE_VALUE)
		;
	else
	{
		if(aio_hs[fd] != INVALID_HANDLE_VALUE)
		{
			msg = "aio_h_set: handle already set!";
			goto fail;
		}
		if(!is_valid_file_handle(h))
		{
			msg = "aio_h_set: setting invalid handle";
			goto fail;
		}
	}

	aio_hs[fd] = h;

	unlock();
	return 0;

fail:
	unlock();
	debug_warn("aio_h_set failed");
	return -1;
}


//////////////////////////////////////////////////////////////////////////////
//
// Req
//
//////////////////////////////////////////////////////////////////////////////


// information about active transfers (reused)
struct Req
{
	// used to identify this request; != 0 <==> request valid
	aiocb* cb;

	OVERLAPPED ovl;
	// hEvent signals when transfer complete

	// read into a separate align buffer if necessary
	// (note: unaligned writes aren't supported. see aio_rw)
	void* buf;		// reused; resized if too small
	size_t buf_size;

	HANDLE hFile;

	size_t pad;		// offset from starting sector
	bool use_align_buffer;
};


// an aiocb is used to pass the request from caller to aio,
// and serves as a "token" identifying the IO - its address is unique.
// Req holds some state needed for the Windows AIO calls (OVERLAPPED).
//
// cb -> req (e.g. in aio_return) is accomplished by searching reqs
// for the given cb (no problem since MAX_REQS is small).
// req -> cb via Req.cb pointer.


const int MAX_REQS = 8;
static Req reqs[MAX_REQS];


static void req_cleanup(void)
{
	Req* r = reqs;

	for(int i = 0; i < MAX_REQS; i++, r++)
	{
		HANDLE& h = r->ovl.hEvent;
		if(h != INVALID_HANDLE_VALUE)
		{
			CloseHandle(h);
			h = INVALID_HANDLE_VALUE;
		}

		::free(r->buf);
		r->buf = 0;
	}
}


static void req_init()
{
	for(int i = 0; i < MAX_REQS; i++)
		reqs[i].ovl.hEvent = CreateEvent(0,1,0,0);	// manual reset

	// buffers are allocated on-demand.
}


// return first Req with given cb field
// (may be 0; useful if searching for a free Req)
static Req* req_find(const aiocb* cb)
{
	Req* r = reqs;
	for(int i = 0; i < MAX_REQS; i++, r++)
		if(r->cb == cb)
			goto found;

	r = 0;	// not found
found:

#ifdef PARANOIA
debug_out("req_find  cb=%p r=%p\n", cb, r);
#endif

	return r;
}


static Req* req_alloc(aiocb* cb)
{
	assert(cb);

	// first free Req, or 0
	Req* r = req_find(0);
	if(r)
		r->cb = cb;

#ifdef PARANOIA
debug_out("req_alloc cb=%p r=%p\n", cb, r);
#endif

	return r;
}


static int req_free(Req* r)
{
#ifdef PARANOIA
debug_out("req_free  cb=%p r=%p\n", r->cb, r);
#endif

	assert(r->cb != 0 && "req_free: not currently in use");
	r->cb = 0;
	return 0;
}


//////////////////////////////////////////////////////////////////////////////
//
// init / cleanup
//
//////////////////////////////////////////////////////////////////////////////


// Win32 functions require sector aligned transfers.
// max of all drives' size is checked in init().
static size_t sector_size = 4096;	// minimum: one page


// caller ensures this is not re-entered!
static int waio_init()
{
	req_init();

	const UINT old_err_mode = SetErrorMode(SEM_FAILCRITICALERRORS);

	// Win32 requires transfers to be sector aligned.
	// find maximum of all drive's sector sizes, then use that.
	// (it's good to know this up-front, and checking every open() is slow).
	const DWORD drives = GetLogicalDrives();
	char drive_str[4] = "?:\\";
	for(int drive = 2; drive <= 26; drive++)	// C: .. Z:
	{
		// avoid BoundsChecker warning by skipping invalid drives
		if(!(drives & (1ul << drive)))
			continue;

		drive_str[0] = (char)('A'+drive);

		DWORD spc, nfc, tnc;	// don't need these
		DWORD sector_size2;
		if(GetDiskFreeSpace(drive_str, &spc, &sector_size2, &nfc, &tnc))
		{
			if(sector_size < sector_size2)
				sector_size = sector_size2;
		}
		// otherwise, it's probably an empty CD drive. ignore the
		// BoundsChecker error; GetDiskFreeSpace seems to be the
		// only way of getting at the sector size.
	}

	SetErrorMode(old_err_mode);

	assert(is_pow2((long)sector_size));

	return 0;
}


static int waio_shutdown()
{
	req_cleanup();
	aio_h_cleanup();
	return 0;
}


int aio_assign_handle(uintptr_t handle)
{
	// 
	// CRT stores osfhandle. if we pass an invalid handle (say, 0),
	// we get an exception when closing the handle if debugging.
	// events can be created relatively quickly (~1800 clocks = 1�s),
	// and are also freed with CloseHandle, so just pass that.
	HANDLE h = CreateEvent(0,0,0,0);
	if(h == INVALID_HANDLE_VALUE)
	{
		debug_warn("aio_assign_handle failed");
		return -1;
	}

	int fd = _open_osfhandle((intptr_t)h, 0);
	if(fd < 0)
	{
		debug_warn("aio_assign_handle failed");
		return fd;
	}

	return aio_h_set(fd, (HANDLE)handle);
}




// open fn in async mode; associate with fd (retrieve via aio_h(fd))
int aio_reopen(int fd, const char* fn, int oflag, ...)
{
	// interpret oflag
	DWORD access = GENERIC_READ;	// assume O_RDONLY
	DWORD share = FILE_SHARE_READ;
	DWORD create = OPEN_EXISTING;
	if(oflag & O_WRONLY)
	{
		access = GENERIC_WRITE;
		share = FILE_SHARE_WRITE;
	}
	else if(oflag & O_RDWR)
	{
		access |= GENERIC_WRITE;
		share |= FILE_SHARE_WRITE;
	}
	if(oflag & O_CREAT)
		create = (oflag & O_EXCL)? CREATE_NEW : CREATE_ALWAYS;

	// open file
	DWORD flags = FILE_FLAG_OVERLAPPED|FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN;
	WIN_SAVE_LAST_ERROR;	// CreateFile
	HANDLE h = CreateFile(fn, access, share, 0, create, flags, 0);
	WIN_RESTORE_LAST_ERROR;
	if(h == INVALID_HANDLE_VALUE)
	{
fail:
		debug_warn("aio_open failed");
		return -1;
	}

	if(aio_h_set(fd, h) < 0)
	{
		CloseHandle(h);
		goto fail;
	}

#ifdef PARANOIA
debug_out("aio_reopen fd=%d fn=%s\n", fd, fn);
#endif

	return 0;
}


int aio_close(int fd)
{
	HANDLE h = aio_h_get(fd);
	if(h == INVALID_HANDLE_VALUE)	// out of bounds or already closed
	{
		debug_warn("aio_close failed");
		return -1;
	}

	if(!CloseHandle(h))
		assert(0);
	aio_h_set(fd, INVALID_HANDLE_VALUE);

#ifdef PARANOIA
debug_out("aio_close fd=%d\n", fd);
#endif

	return 0;
}




// called by aio_read, aio_write, and lio_listio
// cb->aio_lio_opcode specifies desired operation
//
// if cb->aio_fildes doesn't support seeking (e.g. a socket),
// cb->aio_offset must be 0.
static int aio_rw(struct aiocb* cb)
{
	int ret = -1;
	Req* r = 0;

#ifdef PARANOIA
debug_out("aio_rw cb=%p\n", cb);
#endif

	WIN_SAVE_LAST_ERROR;

	// no-op from lio_listio
	if(!cb || cb->aio_lio_opcode == LIO_NOP)
		return 0;

	// fail if aiocb is already in use (forbidden by SUSv3)
	if(req_find(cb))
	{
		debug_warn("aio_rw: aiocb is already in use");
		goto fail;
	}

	// allocate IO request
	r = req_alloc(cb);
	if(!r)
	{
		debug_warn("aio_rw: cannot allocate a Req (too many concurrent IOs)");
		goto fail;
	}

	// extract aiocb fields for convenience
	const bool is_write = (cb->aio_lio_opcode == LIO_WRITE);
	const int fd        = cb->aio_fildes;
	const size_t size   = cb->aio_nbytes;
	const off_t ofs     = cb->aio_offset;
	void* const buf     = (void*)cb->aio_buf; // from volatile void*
	assert(buf);

	HANDLE h = aio_h_get(fd);
	if(h == INVALID_HANDLE_VALUE)
	{
		debug_warn("aio_rw: associated handle is invalid");
		ret = -EINVAL;
		goto fail;
	}
	const bool is_file = (GetFileType(h) == FILE_TYPE_DISK);

	r->hFile = h;
	r->pad = 0;
	r->use_align_buffer = false;

	//
	// align
	//

	size_t actual_ofs = 0;
		// assume socket; if file, set below
	size_t actual_size = size;
	void* actual_buf = buf;

	// leave offset 0 if h is a socket (don't support seeking);
	// otherwise, calculate aligned offset
	if(is_file)
	{
		r->pad = ofs % sector_size;		// offset to start of sector
		actual_ofs = ofs - r->pad;
		actual_size = round_up(size + r->pad, sector_size);

		const bool misaligned = (size != actual_size);
			// either ofs or size is unaligned
		const bool buf_misaligned = ((uintptr_t)buf % sector_size != 0);

		// not aligned
		if(misaligned || buf_misaligned)
		{
			// expand current align buffer if too small
			if(r->buf_size < actual_size)
			{
				void* buf2 = realloc(r->buf, actual_size);
				if(!buf2)
				{
					ret = -ENOMEM;
					goto fail;
				}
				r->buf = buf2;
				r->buf_size = actual_size;
			}

			if(is_write)
			{
				// file offset isn't aligned. we don't support this -
				// we'd have to read padding, then write our data. ugh.
				if(misaligned)
				{
					ret = -EINVAL;
					goto fail;
				}
				// only the buffer is misaligned - copy data to align buffer
				else
					memcpy(r->buf, buf, actual_size);
			}

			actual_buf = r->buf;
			r->use_align_buffer = true;
		}
	}

	// set OVERLAPPED fields
	ResetEvent(r->ovl.hEvent);
	r->ovl.Internal = r->ovl.InternalHigh = 0;
	*(size_t*)&r->ovl.Offset = actual_ofs;
		// HACK: use this instead of OVERLAPPED.Pointer,
		// which isn't defined in older headers (e.g. VC6).
		// 64-bit clean, but endian dependent!

	DWORD size32 = (DWORD)(actual_size & 0xffffffff);
	BOOL ok;
	if(is_write)
		ok = WriteFile(h, actual_buf, size32, 0, &r->ovl);
	else
		ok =  ReadFile(h, actual_buf, size32, 0, &r->ovl);		

	// "pending" isn't an error
	if(GetLastError() == ERROR_IO_PENDING)
		ok = true;

	if(ok)
		ret = 0;

done:
	WIN_RESTORE_LAST_ERROR;

	return ret;

fail:
	req_free(r);
	goto done;
}


int aio_read(struct aiocb* cb)
{
	cb->aio_lio_opcode = LIO_READ;
	return aio_rw(cb);
}


int aio_write(struct aiocb* cb)
{
	cb->aio_lio_opcode = LIO_WRITE;
	return aio_rw(cb);
}


int lio_listio(int mode, struct aiocb* const cbs[], int n, struct sigevent* se)
{
	UNUSED(se)

	int err = 0;

	for(int i = 0; i < n; i++)
	{
		int ret = aio_rw(cbs[i]);		// aio_rw checks for 0 param
		// don't CHECK_ERR - want to try to issue each one
		if(ret < 0)
			err = ret;
	}

	if(err < 0)
		return err;

	if(mode == LIO_WAIT)
		return aio_suspend(cbs, n, 0);

	return 0;
}


// return status of transfer
int aio_error(const struct aiocb* cb)
{
#ifdef PARANOIA
debug_out("aio_error cb=%p\n", cb);
#endif

	Req* const r = req_find(cb);
	if(!r)
		return -1;

	switch(r->ovl.Internal)	// I/O status
	{
	case 0:
		return 0;
	case STATUS_PENDING:
		return EINPROGRESS;

	// TODO: errors
	default:
		return -1;
	}
}


// get bytes transferred. call exactly once for each op.
ssize_t aio_return(struct aiocb* cb)
{
#ifdef PARANOIA
debug_out("aio_return cb=%p\n", cb);
#endif

	Req* const r = req_find(cb);
	if(!r)
	{
		debug_warn("aio_return: cb not found (already called aio_return?)");
		return -1;
	}

	assert(r->ovl.Internal == 0 && "aio_return with transfer in progress");

	BOOL wait = FALSE;	// should already be done!
	DWORD bytes_transferred;
	GetOverlappedResult(r->hFile, &r->ovl, &bytes_transferred, wait);

	// we read into align buffer - copy to user's buffer
	if(r->use_align_buffer)
		memcpy((void*)cb->aio_buf, (u8*)r->buf + r->pad, cb->aio_nbytes);

	// TODO: this copies data back into original buffer from align buffer
	// when writing from unaligned buffer. unnecessarily slow.

	req_free(r);

	return (ssize_t)bytes_transferred;
}


int aio_cancel(int fd, struct aiocb* cb)
{
	UNUSED(cb)

	const HANDLE h = aio_h_get(fd);
	if(h == INVALID_HANDLE_VALUE)
		return -1;

	// Win32 limitation: can't cancel single transfers -
	// all pending reads on this file are cancelled.
	CancelIo(h);
	return AIO_CANCELED;
}


int aio_fsync(int, struct aiocb*)
{
	return -1;
}


int aio_suspend(const struct aiocb* const cbs[], int n, const struct timespec* ts)
{
	int i;

#ifdef PARANOIA
debug_out("aio_suspend cb=%p\n", cbs[0]);
#endif

	if(n <= 0 || n > MAXIMUM_WAIT_OBJECTS)
		return -1;

	int cnt = 0;	// actual number of valid cbs
	HANDLE hs[MAXIMUM_WAIT_OBJECTS];

	for(i = 0; i < n; i++)
	{
		// ignore NULL list entries
		if(!cbs[i])
			continue;

		Req* r = req_find(cbs[i]);
		if(r)
		{
			if(r->ovl.Internal == STATUS_PENDING)
				hs[cnt++] = r->ovl.hEvent;
		}
	}

	// no valid, pending transfers - done
	if(!cnt)
		return 0;

	// timeout: convert timespec to ms (NULL ptr -> no timeout)
	DWORD timeout = INFINITE;
	if(ts)
		timeout = (DWORD)(ts->tv_sec*1000 + ts->tv_nsec/1000000);

	DWORD result = WaitForMultipleObjects(cnt, hs, FALSE, timeout);

	for(i = 0; i < cnt; i++)
		ResetEvent(hs[i]);

	if(result == WAIT_TIMEOUT)
	{
		//errno = -EAGAIN;
		return -1;
	}
	else
		return (result == WAIT_FAILED)? -1 : 0;
}

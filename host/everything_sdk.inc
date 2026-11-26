// Extracted verbatim (by script) from src/mod.cpp lines 214-1175: the
// reduced Everything 1.5 SDK3 client (named pipe) and the reduced
// Everything_IPC.h that the original mod embeds. Copyright (C) 2022-2024
// David Carpenter / voidtools, MIT license (see the notice below).
// Used by host/readiness.cpp for the readiness test query, so the gate talks
// to Everything exactly the way the mod does.
// clang-format off

//
// Copyright (C) 2024 David Carpenter / voidtools
//
// Permission is hereby granted, free of charge,
// to any person obtaining a copy of this software
// and associated documentation files (the "Software"),
// to deal in the Software without restriction,
// including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

// This is an edited version (required code only) of the file in the original Everything 1.5 SDK
// https://www.voidtools.com/forum/viewtopic.php?t=15853

#ifndef EVERYTHING3_BYTE
#define EVERYTHING3_BYTE		BYTE
#endif

#ifndef EVERYTHING3_CHAR
#define EVERYTHING3_CHAR		CHAR
#endif

#ifndef EVERYTHING3_WORD
#define EVERYTHING3_WORD		WORD
#endif

#ifndef EVERYTHING3_DWORD
#define EVERYTHING3_DWORD		DWORD
#endif

#ifndef EVERYTHING3_INT32
#define EVERYTHING3_INT32		__int32
#endif

#ifndef EVERYTHING3_UINT64
#define EVERYTHING3_UINT64		unsigned __int64
#endif

#ifndef EVERYTHING3_SIZE_T
#define EVERYTHING3_SIZE_T		SIZE_T
#endif

#ifndef EVERYTHING3_BOOL
#define EVERYTHING3_BOOL		BOOL
#endif

#ifndef EVERYTHING3_WCHAR
#define EVERYTHING3_WCHAR		WCHAR
#endif

#define EVERYTHING3_DWORD_MAX							0xffffffff
#define EVERYTHING3_UINT64_MAX							0xffffffffffffffffULL

#define EVERYTHING3_OK									0
#define EVERYTHING3_ERROR_OUT_OF_MEMORY					0xE0000001
#define EVERYTHING3_ERROR_IPC_PIPE_NOT_FOUND			0xE0000002
#define EVERYTHING3_ERROR_DISCONNECTED					0xE0000003
#define EVERYTHING3_ERROR_INVALID_PARAMETER				0xE0000004
#define EVERYTHING3_ERROR_BAD_REQUEST					0xE0000005
#define EVERYTHING3_ERROR_CANCELLED						0xE0000006
#define EVERYTHING3_ERROR_PROPERTY_NOT_FOUND			0xE0000007
#define EVERYTHING3_ERROR_SERVER						0xE0000008
#define EVERYTHING3_ERROR_INVALID_COMMAND				0xE0000009
#define EVERYTHING3_ERROR_BAD_RESPONSE					0xE000000A
#define EVERYTHING3_ERROR_INSUFFICIENT_BUFFER			0xE000000B
#define EVERYTHING3_ERROR_SHUTDOWN						0xE000000C

typedef EVERYTHING3_BYTE EVERYTHING3_UTF8;
typedef struct _everything3_client_s EVERYTHING3_CLIENT;

#ifndef EVERYTHING3_API
#define EVERYTHING3_API //__stdcall
#endif

EVERYTHING3_CLIENT *EVERYTHING3_API Everything3_ConnectW(const EVERYTHING3_WCHAR *instance_name);
EVERYTHING3_BOOL EVERYTHING3_API Everything3_ShutdownClient(EVERYTHING3_CLIENT *client);
EVERYTHING3_BOOL EVERYTHING3_API Everything3_DestroyClient(EVERYTHING3_CLIENT *client);

EVERYTHING3_UINT64 EVERYTHING3_API Everything3_GetFolderSizeFromFilenameW(EVERYTHING3_CLIENT *client, const EVERYTHING3_WCHAR *lpFilename);

//
// Copyright (C) 2024 David Carpenter / voidtools
//
// Permission is hereby granted, free of charge,
// to any person obtaining a copy of this software
// and associated documentation files (the "Software"),
// to deal in the Software without restriction,
// including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

// This is an edited version (required code only) of the file in the original Everything 1.5 SDK
// https://www.voidtools.com/forum/viewtopic.php?t=15853

// #define WIN32_LEAN_AND_MEAN

// #include <windows.h>

// #include "Everything3.h"

#define _EVERYTHING3_WCHAR_BUF_STACK_SIZE					MAX_PATH
#define _EVERYTHING3_UTF8_BUF_STACK_SIZE					MAX_PATH

// IPC pipe commands.
#define _EVERYTHING3_COMMAND_GET_FOLDER_SIZE				18

// IPC pipe responses
#define _EVERYTHING3_RESPONSE_OK_MORE_DATA					100
#define _EVERYTHING3_RESPONSE_OK							200
#define _EVERYTHING3_RESPONSE_ERROR_BAD_REQUEST				400
#define _EVERYTHING3_RESPONSE_ERROR_CANCELLED				401
#define _EVERYTHING3_RESPONSE_ERROR_NOT_FOUND				404
#define _EVERYTHING3_RESPONSE_ERROR_OUT_OF_MEMORY			500
#define _EVERYTHING3_RESPONSE_ERROR_INVALID_COMMAND			501

typedef struct _everything3_wchar_buf_s {
	EVERYTHING3_WCHAR *buf;
	SIZE_T length_in_wchars;
	SIZE_T size_in_wchars;
	EVERYTHING3_WCHAR stack_buf[_EVERYTHING3_WCHAR_BUF_STACK_SIZE];
} _everything3_wchar_buf_t;

typedef struct _everything3_utf8_buf_s {
	EVERYTHING3_UTF8 *buf;
	SIZE_T length_in_bytes;
	SIZE_T size_in_bytes;
	EVERYTHING3_UTF8 stack_buf[_EVERYTHING3_UTF8_BUF_STACK_SIZE];
} _everything3_utf8_buf_t;

typedef struct _everything3_message_s {
	DWORD code;
	DWORD size;
} _everything3_message_t;

typedef struct _everything3_client_s {
	CRITICAL_SECTION cs;
	HANDLE pipe_handle;
	HANDLE send_event;
	HANDLE recv_event;
	HANDLE shutdown_event;
} _everything3_client_t;

static void _everything3_Lock(_everything3_client_t *client);
static void _everything3_Unlock(_everything3_client_t *client);
static void *_everything3_mem_alloc(SIZE_T size);
static void *_everything3_mem_calloc(SIZE_T size);
static void _everything3_mem_free(void *ptr);
static SIZE_T _everything3_safe_size_add(SIZE_T a, SIZE_T b);
static EVERYTHING3_WCHAR *_everything3_wchar_string_cat_wchar_string_no_null_terminate(EVERYTHING3_WCHAR *buf, EVERYTHING3_WCHAR *current_d, const EVERYTHING3_WCHAR *s);
static void _everything3_wchar_buf_init(_everything3_wchar_buf_t *wcbuf);
static void _everything3_wchar_buf_kill(_everything3_wchar_buf_t *wcbuf);
static void _everything3_wchar_buf_empty(_everything3_wchar_buf_t *wcbuf);
static BOOL _everything3_wchar_buf_grow_size(_everything3_wchar_buf_t *wcbuf, SIZE_T length_in_wchars);
static BOOL _everything3_wchar_buf_grow_length(_everything3_wchar_buf_t *wcbuf, SIZE_T length_in_wchars);
static BOOL _everything3_wchar_buf_get_pipe_name(_everything3_wchar_buf_t *wcbuf, const EVERYTHING3_WCHAR *instance_name);
static BOOL _everything3_send(_everything3_client_t *client, DWORD code, const void *in_data, SIZE_T in_size);
static BOOL _everything3_recv_header(_everything3_client_t *client, _everything3_message_t *recv_header);
static BOOL _everything3_recv_data(_everything3_client_t *client, void *buf, SIZE_T buf_size);
static BOOL _everything3_recv_skip(_everything3_client_t *client, SIZE_T size);
static BOOL _everything3_ioctrl(_everything3_client_t *client, DWORD code, const void *in_data, SIZE_T in_size, void *out_data, SIZE_T out_size, SIZE_T *out_numread);
static void _everything3_utf8_buf_init(_everything3_utf8_buf_t *wcbuf);
static void _everything3_utf8_buf_kill(_everything3_utf8_buf_t *wcbuf);
static void _everything3_utf8_buf_empty(_everything3_utf8_buf_t *wcbuf);
static BOOL _everything3_utf8_buf_grow_size(_everything3_utf8_buf_t *wcbuf, SIZE_T size_in_bytes);
static BOOL _everything3_utf8_buf_grow_length(_everything3_utf8_buf_t *wcbuf, SIZE_T length_in_bytes);
static BOOL _everything3_utf8_buf_copy_wchar_string(_everything3_utf8_buf_t *cbuf, const EVERYTHING3_WCHAR *ws);
static EVERYTHING3_UTF8 *_everything3_utf8_string_copy_wchar_string(EVERYTHING3_UTF8 *buf, const EVERYTHING3_WCHAR *ws);

static EVERYTHING3_WCHAR *_everything3_wchar_string_cat_wchar_string_no_null_terminate(EVERYTHING3_WCHAR *buf, EVERYTHING3_WCHAR *current_d, const EVERYTHING3_WCHAR *s) {
	const EVERYTHING3_WCHAR *p;
	EVERYTHING3_WCHAR *d;

	p = s;
	d = current_d;

	while (*p) {
		if (buf) {
			*d++ = *p;
		} else {
			d = (WCHAR *)_everything3_safe_size_add((SIZE_T)d, 1);
		}

		p++;
	}

	return d;
}

static EVERYTHING3_WCHAR *_Everything3_get_pipe_name(EVERYTHING3_WCHAR *buf, const EVERYTHING3_WCHAR *instance_name) {
	EVERYTHING3_WCHAR *d;

	d = buf;

	d = _everything3_wchar_string_cat_wchar_string_no_null_terminate(buf, d, L"\\\\.\\PIPE\\Everything IPC");

	if ((instance_name) && (*instance_name)) {
		d = _everything3_wchar_string_cat_wchar_string_no_null_terminate(buf, d, L" (");
		d = _everything3_wchar_string_cat_wchar_string_no_null_terminate(buf, d, instance_name);
		d = _everything3_wchar_string_cat_wchar_string_no_null_terminate(buf, d, L")");
	}

	if (buf) {
		*d = 0;
	}

	return d;
}

static BOOL _everything3_wchar_buf_get_pipe_name(_everything3_wchar_buf_t *wcbuf, const EVERYTHING3_WCHAR *instance_name) {
	if (_everything3_wchar_buf_grow_length(wcbuf, (SIZE_T)_Everything3_get_pipe_name(NULL, instance_name))) {
		_Everything3_get_pipe_name(wcbuf->buf, instance_name);

		return TRUE;
	}

	return FALSE;
}

// connects to the named pipe "\\\\.\\PIPE\\Everything IPC"
// instance_name can BE NULL.
// a NULL instance_name or an empty instance_name will connect to the unnamed instance.
// The Everything 1.5a release will use an "1.5a" instance.
_everything3_client_t *EVERYTHING3_API Everything3_ConnectW(const EVERYTHING3_WCHAR *instance_name) {
	_everything3_client_t *ret;
	_everything3_wchar_buf_t pipe_name_wcbuf;

	ret = NULL;
	_everything3_wchar_buf_init(&pipe_name_wcbuf);

	if (_everything3_wchar_buf_get_pipe_name(&pipe_name_wcbuf, instance_name)) {
		HANDLE pipe_handle;

		pipe_handle = CreateFileW(pipe_name_wcbuf.buf, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
		if (pipe_handle != INVALID_HANDLE_VALUE) {
			_everything3_client_t *client;

			client = (_everything3_client_t *)_everything3_mem_calloc(sizeof(_everything3_client_t));

			if (client) {
				InitializeCriticalSection(&client->cs);

				client->pipe_handle = pipe_handle;

				pipe_handle = INVALID_HANDLE_VALUE;

				client->shutdown_event = CreateEvent(0, TRUE, FALSE, 0);
				if (client->shutdown_event) {
					client->send_event = CreateEvent(0, TRUE, FALSE, 0);
					if (client->send_event) {
						client->recv_event = CreateEvent(0, TRUE, FALSE, 0);
						if (client->recv_event) {
							ret = client;
							client = NULL;
						} else {
							SetLastError(EVERYTHING3_ERROR_OUT_OF_MEMORY);
						}
					} else {
						SetLastError(EVERYTHING3_ERROR_OUT_OF_MEMORY);
					}
				} else {
					SetLastError(EVERYTHING3_ERROR_OUT_OF_MEMORY);
				}

				if (client) {
					Everything3_DestroyClient(client);
				}
			} else {
				SetLastError(EVERYTHING3_ERROR_OUT_OF_MEMORY);
			}

			if (pipe_handle != INVALID_HANDLE_VALUE) {
				CloseHandle(pipe_handle);
			}

		} else {
			SetLastError(EVERYTHING3_ERROR_IPC_PIPE_NOT_FOUND);
		}
	} else {
		SetLastError(EVERYTHING3_ERROR_OUT_OF_MEMORY);
	}

	_everything3_wchar_buf_kill(&pipe_name_wcbuf);

	return ret;
}

BOOL EVERYTHING3_API Everything3_ShutdownClient(_everything3_client_t *client) {
	if (client) {
		SetEvent(client->shutdown_event);

		return TRUE;
	}

	SetLastError(EVERYTHING3_ERROR_INVALID_PARAMETER);

	return FALSE;
}

BOOL EVERYTHING3_API Everything3_DestroyClient(_everything3_client_t *client) {
	if (client) {
		Everything3_ShutdownClient(client);

		if (client->recv_event) {
			CloseHandle(client->recv_event);
		}

		if (client->send_event) {
			CloseHandle(client->send_event);
		}

		if (client->shutdown_event) {
			CloseHandle(client->shutdown_event);
		}

		if (client->pipe_handle != INVALID_HANDLE_VALUE) {
			CloseHandle(client->pipe_handle);

			client->pipe_handle = INVALID_HANDLE_VALUE;
		}

		DeleteCriticalSection(&client->cs);

		_everything3_mem_free(client);

		return TRUE;
	} else {
		SetLastError(EVERYTHING3_ERROR_INVALID_PARAMETER);

		return FALSE;
	}
}

static void *_everything3_mem_alloc(SIZE_T size) {
	void *p;

	if (size == SIZE_MAX) {
		return NULL;
	}

	p = HeapAlloc(GetProcessHeap(), 0, size);

	if (!p) {
		SetLastError(EVERYTHING3_ERROR_OUT_OF_MEMORY);
	}

	return p;
}

static void *_everything3_mem_calloc(SIZE_T size) {
	void *p;

	p = _everything3_mem_alloc(size);
	if (p) {
		ZeroMemory(p, size);
	}

	return p;
}

static void _everything3_mem_free(void *ptr) {
	HeapFree(GetProcessHeap(), 0, ptr);
}

SIZE_T _everything3_safe_size_add(SIZE_T a, SIZE_T b) {
	SIZE_T c;

	c = a + b;

	if (c < a) {
		return SIZE_MAX;
	}

	return c;
}

static void _everything3_Lock(_everything3_client_t *client) {
	EnterCriticalSection(&client->cs);
}

static void _everything3_Unlock(_everything3_client_t *client) {
	LeaveCriticalSection(&client->cs);
}

static void _everything3_wchar_buf_init(_everything3_wchar_buf_t *wcbuf) {
	wcbuf->buf = wcbuf->stack_buf;
	wcbuf->length_in_wchars = 0;
	wcbuf->size_in_wchars = _EVERYTHING3_WCHAR_BUF_STACK_SIZE;
	wcbuf->buf[0] = 0;
}

static void _everything3_wchar_buf_kill(_everything3_wchar_buf_t *wcbuf) {
	if (wcbuf->buf != wcbuf->stack_buf) {
		_everything3_mem_free(wcbuf->buf);
	}
}

static void _everything3_wchar_buf_empty(_everything3_wchar_buf_t *wcbuf) {
	_everything3_wchar_buf_kill(wcbuf);
	_everything3_wchar_buf_init(wcbuf);
}

static BOOL _everything3_wchar_buf_grow_size(_everything3_wchar_buf_t *wcbuf, SIZE_T size_in_wchars) {
	EVERYTHING3_WCHAR *new_buf;

	if (size_in_wchars <= wcbuf->size_in_wchars) {
		return TRUE;
	}

	_everything3_wchar_buf_empty(wcbuf);

	new_buf = (WCHAR *)_everything3_mem_alloc(_everything3_safe_size_add(size_in_wchars, size_in_wchars));

	if (new_buf) {
		wcbuf->buf = new_buf;
		wcbuf->size_in_wchars = size_in_wchars;

		return TRUE;
	}

	return FALSE;
}

static BOOL _everything3_wchar_buf_grow_length(_everything3_wchar_buf_t *wcbuf, SIZE_T length_in_wchars) {
	if (_everything3_wchar_buf_grow_size(wcbuf, _everything3_safe_size_add(length_in_wchars, 1))) {
		wcbuf->length_in_wchars = length_in_wchars;

		return TRUE;
	}

	return FALSE;
}

static BOOL _everything3_send(_everything3_client_t *client, DWORD code, const void *in_data, SIZE_T in_size) {
	if (in_size <= EVERYTHING3_DWORD_MAX) {
		_everything3_message_t send_message;
		DWORD numwritten;
		OVERLAPPED send_overlapped;
		BYTE *send_p;
		DWORD send_run;

		send_message.code = code;
		send_message.size = (DWORD)in_size;

		send_overlapped.hEvent = client->send_event;
		send_overlapped.Offset = 0;
		send_overlapped.OffsetHigh = 0;

		send_p = (BYTE *)&send_message;
		send_run = sizeof(send_message);

		while (send_run) {
			if (WriteFile(client->pipe_handle, send_p, send_run, &numwritten, &send_overlapped)) {
				if (numwritten) {
					send_p += numwritten;
					send_run -= numwritten;

					continue;
				} else {
					SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

					return FALSE;
				}
			} else {
				DWORD last_error;

				last_error = GetLastError();

				if ((last_error == ERROR_IO_INCOMPLETE) || (last_error == ERROR_IO_PENDING)) {
					HANDLE wait_handles[2];

					wait_handles[0] = client->shutdown_event;
					wait_handles[1] = client->send_event;

					if (WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
						SetLastError(EVERYTHING3_ERROR_SHUTDOWN);

						return FALSE;
					}

					if (GetOverlappedResult(client->pipe_handle, &send_overlapped, &numwritten, FALSE)) {
						if (numwritten) {
							send_p += numwritten;
							send_run -= numwritten;

							continue;
						} else {
							SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

							return FALSE;
						}
					} else {
						SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

						return FALSE;
					}
				} else {
					SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

					return FALSE;
				}
			}
		}

		send_p = (BYTE *)in_data;
		send_run = (DWORD)in_size;

		while (send_run) {
			if (WriteFile(client->pipe_handle, send_p, send_run, &numwritten, &send_overlapped)) {
				send_p += numwritten;
				send_run -= numwritten;

				continue;
			} else {
				DWORD last_error;

				last_error = GetLastError();

				if ((last_error == ERROR_IO_INCOMPLETE) || (last_error == ERROR_IO_PENDING)) {
					HANDLE wait_handles[2];

					wait_handles[0] = client->shutdown_event;
					wait_handles[1] = client->send_event;

					if (WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
						SetLastError(EVERYTHING3_ERROR_SHUTDOWN);

						return FALSE;
					}

					if (GetOverlappedResult(client->pipe_handle, &send_overlapped, &numwritten, FALSE)) {
						send_p += numwritten;
						send_run -= numwritten;

						continue;
					} else {
						SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

						return FALSE;
					}
				} else {
					SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

					return FALSE;
				}
			}
		}

		return TRUE;
	} else {
		SetLastError(EVERYTHING3_ERROR_OUT_OF_MEMORY);

		return FALSE;
	}
}

static BOOL _everything3_recv_header(_everything3_client_t *client, _everything3_message_t *recv_header) {
	BOOL ret;

	ret = FALSE;

	if (_everything3_recv_data(client, recv_header, sizeof(_everything3_message_t))) {
		if ((recv_header->code == _EVERYTHING3_RESPONSE_OK) || (recv_header->code == _EVERYTHING3_RESPONSE_OK_MORE_DATA)) {
			ret = TRUE;
		} else {
			if (_everything3_recv_skip(client, recv_header->size)) {
				if (recv_header->code == _EVERYTHING3_RESPONSE_ERROR_BAD_REQUEST) {
					SetLastError(EVERYTHING3_ERROR_BAD_REQUEST);
				} else if (recv_header->code == _EVERYTHING3_RESPONSE_ERROR_CANCELLED) {
					SetLastError(EVERYTHING3_ERROR_CANCELLED);
				} else if (recv_header->code == _EVERYTHING3_RESPONSE_ERROR_NOT_FOUND) {
					SetLastError(EVERYTHING3_ERROR_IPC_PIPE_NOT_FOUND);
				} else if (recv_header->code == _EVERYTHING3_RESPONSE_ERROR_OUT_OF_MEMORY) {
					SetLastError(EVERYTHING3_ERROR_SERVER);
				} else if (recv_header->code == _EVERYTHING3_RESPONSE_ERROR_INVALID_COMMAND) {
					SetLastError(EVERYTHING3_ERROR_INVALID_COMMAND);
				} else {
					SetLastError(EVERYTHING3_ERROR_BAD_RESPONSE);
				}
			}
		}
	}

	return ret;
}

static BOOL _everything3_recv_data(_everything3_client_t *client, void *buf, SIZE_T buf_size) {
	BOOL ret;
	DWORD numread;
	OVERLAPPED recv_overlapped;
	BYTE *recv_p;
	SIZE_T recv_run;

	ret = FALSE;
	recv_overlapped.hEvent = client->recv_event;
	recv_overlapped.Offset = 0;
	recv_overlapped.OffsetHigh = 0;

	recv_p = (BYTE *)buf;
	recv_run = buf_size;

	for (;;) {
		DWORD chunk_size;

		if (!recv_run) {
			ret = TRUE;

			break;
		}

		if (recv_run <= 65536) {
			chunk_size = (DWORD)recv_run;
		} else {
			chunk_size = 65536;
		}

		if (ReadFile(client->pipe_handle, recv_p, chunk_size, &numread, &recv_overlapped)) {
			if (numread) {
				recv_p += numread;
				recv_run -= numread;
			} else {
				SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

				break;
			}
		} else {
			DWORD last_error;

			last_error = GetLastError();

			if ((last_error == ERROR_IO_INCOMPLETE) || (last_error == ERROR_IO_PENDING)) {
				HANDLE wait_handles[2];

				wait_handles[0] = client->shutdown_event;
				wait_handles[1] = client->recv_event;

				if (WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
					SetLastError(EVERYTHING3_ERROR_SHUTDOWN);

					break;
				}

				if (GetOverlappedResult(client->pipe_handle, &recv_overlapped, &numread, FALSE)) {
					if (numread) {
						recv_p += numread;
						recv_run -= numread;
					} else {
						SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

						break;
					}
				} else {
					SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

					break;
				}
			} else {
				SetLastError(EVERYTHING3_ERROR_DISCONNECTED);

				break;
			}
		}
	}

	return ret;
}

static BOOL _everything3_recv_skip(_everything3_client_t *client, SIZE_T size) {
	BOOL ret;
	BYTE buf[256];
	SIZE_T run;

	ret = FALSE;
	run = size;

	for (;;) {
		SIZE_T recv_size;

		if (!run) {
			ret = TRUE;
			break;
		}

		recv_size = run;
		if (recv_size > 256) {
			recv_size = 256;
		}

		if (!_everything3_recv_data(client, buf, recv_size)) {
			break;
		}

		run -= recv_size;
	}

	return ret;
}


static BOOL _everything3_ioctrl(_everything3_client_t *client, DWORD code, const void *in_data, SIZE_T in_size, void *out_data, SIZE_T out_size, SIZE_T *out_numread) {
	BOOL ret;

	ret = FALSE;

	if (client) {
		_everything3_Lock(client);

		if (_everything3_send(client, code, in_data, in_size)) {
			_everything3_message_t recv_header;

			if (_everything3_recv_header(client, &recv_header)) {
				if (recv_header.size <= out_size) {
					if (_everything3_recv_data(client, out_data, recv_header.size)) {
						if (out_numread) {
							*out_numread = recv_header.size;
						}

						ret = TRUE;
					}
				} else {
					if (_everything3_recv_skip(client, recv_header.size)) {
						SetLastError(EVERYTHING3_ERROR_INSUFFICIENT_BUFFER);
					}
				}
			}
		}

		_everything3_Unlock(client);
	} else {
		SetLastError(EVERYTHING3_ERROR_INVALID_PARAMETER);
	}

	return ret;
}

static void _everything3_utf8_buf_init(_everything3_utf8_buf_t *cbuf) {
	cbuf->buf = cbuf->stack_buf;
	cbuf->length_in_bytes = 0;
	cbuf->size_in_bytes = _EVERYTHING3_UTF8_BUF_STACK_SIZE;
	cbuf->buf[0] = 0;
}

static void _everything3_utf8_buf_kill(_everything3_utf8_buf_t *cbuf) {
	if (cbuf->buf != cbuf->stack_buf) {
		_everything3_mem_free(cbuf->buf);
	}
}

static void _everything3_utf8_buf_empty(_everything3_utf8_buf_t *cbuf) {
	_everything3_utf8_buf_kill(cbuf);
	_everything3_utf8_buf_init(cbuf);
}

static BOOL _everything3_utf8_buf_grow_size(_everything3_utf8_buf_t *cbuf, SIZE_T size_in_bytes) {
	BYTE *new_buf;

	if (size_in_bytes <= cbuf->size_in_bytes) {
		return TRUE;
	}

	_everything3_utf8_buf_empty(cbuf);

	new_buf = (BYTE *)_everything3_mem_alloc(size_in_bytes);

	if (new_buf) {
		cbuf->buf = new_buf;
		cbuf->size_in_bytes = size_in_bytes;

		return TRUE;
	}

	return FALSE;
}

static BOOL _everything3_utf8_buf_grow_length(_everything3_utf8_buf_t *cbuf, SIZE_T length_in_bytes) {
	if (_everything3_utf8_buf_grow_size(cbuf, _everything3_safe_size_add(length_in_bytes, 1))) {
		cbuf->length_in_bytes = length_in_bytes;

		return TRUE;
	}

	return FALSE;
}

static EVERYTHING3_UTF8 *_everything3_utf8_string_copy_wchar_string(EVERYTHING3_UTF8 *buf, const EVERYTHING3_WCHAR *ws) {
	const EVERYTHING3_WCHAR *p;
	EVERYTHING3_UTF8 *d;
	int c;

	p = ws;
	d = buf;

	while (*p) {
		c = *p++;

		// surrogates
		if ((c >= 0xD800) && (c < 0xDC00)) {
			if ((*p >= 0xDC00) && (*p < 0xE000)) {
				c = 0x10000 + ((c - 0xD800) << 10) + (*p - 0xDC00);

				p++;
			}
		}

		if (c > 0xffff) {
			// 4 bytes
			if (buf) {
				*d++ = ((c >> 18) & 0x07) | 0xF0; // 11110xxx
				*d++ = ((c >> 12) & 0x3f) | 0x80; // 10xxxxxx
				*d++ = ((c >> 6) & 0x3f) | 0x80; // 10xxxxxx
				*d++ = (c & 0x3f) | 0x80; // 10xxxxxx
			} else {
				d = (EVERYTHING3_UTF8 *)_everything3_safe_size_add((SIZE_T)d, 4);
			}
		} else if (c > 0x7ff) {
			// 3 bytes
			if (buf) {
				*d++ = ((c >> 12) & 0x0f) | 0xE0; // 1110xxxx
				*d++ = ((c >> 6) & 0x3f) | 0x80; // 10xxxxxx
				*d++ = (c & 0x3f) | 0x80; // 10xxxxxx
			} else {
				d = (EVERYTHING3_UTF8 *)_everything3_safe_size_add((SIZE_T)d, 3);
			}
		} else if (c > 0x7f) {
			// 2 bytes
			if (buf) {
				*d++ = ((c >> 6) & 0x1f) | 0xC0; // 110xxxxx
				*d++ = (c & 0x3f) | 0x80; // 10xxxxxx
			} else {
				d = (EVERYTHING3_UTF8 *)_everything3_safe_size_add((SIZE_T)d, 2);
			}
		} else {
			// ascii
			if (buf) {
				*d++ = c;
			} else {
				d = (EVERYTHING3_UTF8 *)_everything3_safe_size_add((SIZE_T)d, 1);
			}
		}
	}

	if (buf) {
		*d = 0;
	}

	return d;
}

static BOOL _everything3_utf8_buf_copy_wchar_string(_everything3_utf8_buf_t *cbuf, const EVERYTHING3_WCHAR *ws) {
	if (_everything3_utf8_buf_grow_length(cbuf, (SIZE_T)_everything3_utf8_string_copy_wchar_string(NULL, ws))) {
		_everything3_utf8_string_copy_wchar_string(cbuf->buf, ws);

		return TRUE;
	}

	return FALSE;
}


static EVERYTHING3_UINT64 _everything3_get_folder_size(EVERYTHING3_CLIENT *client, const EVERYTHING3_UTF8 *filename, SIZE_T filename_length_in_bytes) {
	EVERYTHING3_UINT64 ret;
	EVERYTHING3_UINT64 value;
	SIZE_T numread;

	ret = EVERYTHING3_UINT64_MAX;

	if (_everything3_ioctrl(client, _EVERYTHING3_COMMAND_GET_FOLDER_SIZE, filename, filename_length_in_bytes, &value, sizeof(EVERYTHING3_UINT64), &numread)) {
		if (numread == sizeof(EVERYTHING3_UINT64)) {
			ret = value;

			if (ret == EVERYTHING3_UINT64_MAX) {
				SetLastError(EVERYTHING3_OK);
			}
		} else {
			SetLastError(EVERYTHING3_ERROR_BAD_RESPONSE);
		}
	}

	return ret;
}

EVERYTHING3_UINT64 EVERYTHING3_API Everything3_GetFolderSizeFromFilenameW(EVERYTHING3_CLIENT *client, LPCWSTR lpFilename) {
	EVERYTHING3_UINT64 ret;
	_everything3_utf8_buf_t filename_cbuf;

	ret = EVERYTHING3_UINT64_MAX;

	_everything3_utf8_buf_init(&filename_cbuf);

	if (_everything3_utf8_buf_copy_wchar_string(&filename_cbuf, lpFilename)) {
		ret = _everything3_get_folder_size(client, filename_cbuf.buf, filename_cbuf.length_in_bytes);
	}

	_everything3_utf8_buf_kill(&filename_cbuf);

	return ret;
}

// Severely reduced Everything_IPC.h, Copyright (C) 2022 David Carpenter
// https://www.voidtools.com/Everything-SDK.zip

#define EVERYTHING_IPC_WNDCLASSW				L"EVERYTHING_TASKBAR_NOTIFICATION"
#define EVERYTHING_IPC_WNDCLASSW_15A			L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)"

#define EVERYTHING_IPC_MATCHCASE				0x00000001
#define EVERYTHING_IPC_MATCHDIACRITICS			0x00000010
#define EVERYTHING_IPC_COPYDATA_QUERY2W			18
#define EVERYTHING_IPC_SORT_NAME_ASCENDING		1

#define EVERYTHING_IPC_QUERY2_REQUEST_SIZE		0x00000010

typedef struct {
	DWORD reply_hwnd;
	DWORD reply_copydata_message;
	DWORD search_flags;
	DWORD offset;
	DWORD max_results;
	DWORD request_flags;
	DWORD sort_type;
} EVERYTHING_IPC_QUERY2;

typedef struct {
	DWORD totitems;
	DWORD numitems;
	DWORD offset;
	DWORD request_flags;
	DWORD sort_type;
} EVERYTHING_IPC_LIST2;

typedef struct {
	DWORD flags;
	DWORD data_offset;
} EVERYTHING_IPC_ITEM2;

// clang-format on

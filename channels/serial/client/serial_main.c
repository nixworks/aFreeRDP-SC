/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Serial Port Device Service Virtual Channel
 *
 * Copyright 2011 O.S. Systems Software Ltda.
 * Copyright 2011 Eduardo Fiss Beloni <beloni@ossystems.com.br>
 * Copyright 2014 Hewlett-Packard Development Company, L.P.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SYS_MODEM_H
#include <sys/modem.h>
#endif
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#ifdef HAVE_SYS_STRTIO_H
#include <sys/strtio.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <winpr/collections.h>
#include <winpr/comm.h>
#include <winpr/crt.h>
#include <winpr/stream.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/wlog.h>

#include <winpr/wlog.h>

#include <freerdp/freerdp.h>
#include <freerdp/utils/list.h>
#include <freerdp/channels/rdpdr.h>
#include <freerdp/utils/svc_plugin.h>

#define MAX_IRP_THREADS	5

typedef struct _SERIAL_DEVICE SERIAL_DEVICE;

struct _SERIAL_DEVICE
{
	DEVICE device;
	HANDLE* hComm;

	/* TODO: use of log (prefered the old fashion DEBUG_SVC and
	 * DEBUG_WARN macros for backward compatibility resaons)
	 */
	wLog* log;
	HANDLE MainThread;
	wMessageQueue* MainIrpQueue;

	/* one thread per pending IRP and indexed according their CompletionId */
	wListDictionary *IrpThreads;
	UINT32 IrpThreadToBeTerminatedCount;
	CRITICAL_SECTION TerminatingIrpThreadsLock;
};

typedef struct _IRP_THREAD_DATA IRP_THREAD_DATA;

struct _IRP_THREAD_DATA
{
	SERIAL_DEVICE *serial;
	IRP *irp;
};


static void serial_process_irp_create(SERIAL_DEVICE* serial, IRP* irp)
{
	DWORD DesiredAccess;
	DWORD SharedAccess;
	DWORD CreateDisposition;
	UINT32 PathLength;

	Stream_Read_UINT32(irp->input, DesiredAccess);		/* DesiredAccess (4 bytes) */
	Stream_Seek_UINT64(irp->input);				/* AllocationSize (8 bytes) */
	Stream_Seek_UINT32(irp->input);				/* FileAttributes (4 bytes) */
	Stream_Read_UINT32(irp->input, SharedAccess);		/* SharedAccess (4 bytes) */
	Stream_Read_UINT32(irp->input, CreateDisposition);	/* CreateDisposition (4 bytes) */
	Stream_Seek_UINT32(irp->input); 			/* CreateOptions (4 bytes) */
	Stream_Read_UINT32(irp->input, PathLength);		/* PathLength (4 bytes) */
	Stream_Seek(irp->input, PathLength);			/* Path (variable) */

	assert(PathLength == 0); /* MS-RDPESP 2.2.2.2 */


#ifndef _WIN32
	/* Windows 2012 server sends on a first call : 
	 *     DesiredAccess     = 0x00100080: SYNCHRONIZE | FILE_READ_ATTRIBUTES
	 *     SharedAccess      = 0x00000007: FILE_SHARE_DELETE | FILE_SHARE_WRITE | FILE_SHARE_READ
	 *     CreateDisposition = 0x00000001: CREATE_NEW
	 *
	 * then Windows 2012 sends : 
	 *     DesiredAccess     = 0x00120089: SYNCHRONIZE | READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_READ_EA | FILE_READ_DATA
	 *     SharedAccess      = 0x00000007: FILE_SHARE_DELETE | FILE_SHARE_WRITE | FILE_SHARE_READ
	 *     CreateDisposition = 0x00000001: CREATE_NEW
	 *
	 * assert(DesiredAccess == (GENERIC_READ | GENERIC_WRITE));
	 * assert(SharedAccess == 0);
	 * assert(CreateDisposition == OPEN_EXISTING);
	 *
	 */

	DEBUG_SVC("DesiredAccess: 0x%0.8x, SharedAccess: 0x%0.8x, CreateDisposition: 0x%0.8x", DesiredAccess, SharedAccess, CreateDisposition);

	/* FIXME: As of today only the flags below are supported by CommCreateFileA: */
	DesiredAccess     = GENERIC_READ | GENERIC_WRITE;
	SharedAccess      = 0;
	CreateDisposition = OPEN_EXISTING;
#endif

	serial->hComm = CreateFile(serial->device.name,
				   DesiredAccess,	
				   SharedAccess,	
				   NULL,		/* SecurityAttributes */
				   CreateDisposition,	
				   0,			/* FlagsAndAttributes */
				   NULL);		/* TemplateFile */

	if (!serial->hComm || (serial->hComm == INVALID_HANDLE_VALUE))
	{
		DEBUG_WARN("CreateFile failure: %s last-error: Ox%lX\n", serial->device.name, GetLastError());

		irp->IoStatus = STATUS_UNSUCCESSFUL;
		goto error_handle;
	}

	/* FIXME: Appeared to be useful to setup some devices. Guess
	 * the device driver asked to setup some unsupported feature
	 * that were not eventually used. TODO: collecting more
	 * details, a command line argument? */
	/* _comm_set_permissive(serial->hComm, TRUE); */

	/* NOTE: binary mode/raw mode required for the redirection. On
	 * Linux, CommCreateFileA forces this setting.
	 */
	/* ZeroMemory(&dcb, sizeof(DCB)); */
	/* dcb.DCBlength = sizeof(DCB); */
	/* GetCommState(serial->hComm, &dcb); */
	/* dcb.fBinary = TRUE; */
	/* SetCommState(serial->hComm, &dcb); */

	// TMP:
	COMMTIMEOUTS timeouts;
	timeouts.ReadIntervalTimeout = MAXULONG; /* ensures a non blocking state */
	timeouts.ReadTotalTimeoutMultiplier = 0;
	timeouts.ReadTotalTimeoutConstant = 0;
	timeouts.WriteTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant = 0;
	SetCommTimeouts(serial->hComm, &timeouts);

	assert(irp->FileId == 0);
	irp->FileId = irp->devman->id_sequence++; /* FIXME: why not ((WINPR_COMM*)hComm)->fd? */

	irp->IoStatus = STATUS_SUCCESS;

	DEBUG_SVC("%s (DeviceId: %d, FileId: %d) created.", serial->device.name, irp->device->id, irp->FileId);

  error_handle:
	Stream_Write_UINT32(irp->output, irp->FileId);	/* FileId (4 bytes) */
	Stream_Write_UINT8(irp->output, 0);		/* Information (1 byte) */
}

static void serial_process_irp_close(SERIAL_DEVICE* serial, IRP* irp)
{
	Stream_Seek(irp->input, 32); /* Padding (32 bytes) */

	if (!CloseHandle(serial->hComm))
	{
		DEBUG_WARN("CloseHandle failure: %s (%d) closed.", serial->device.name, irp->device->id);
		irp->IoStatus = STATUS_UNSUCCESSFUL;
		goto error_handle;
	}

	DEBUG_SVC("%s (DeviceId: %d, FileId: %d) closed.", serial->device.name, irp->device->id, irp->FileId);

	serial->hComm = NULL;
	irp->IoStatus = STATUS_SUCCESS;

  error_handle:
	Stream_Zero(irp->output, 5); /* Padding (5 bytes) */
}

static void serial_process_irp_read(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 Length;
	UINT64 Offset;
	BYTE* buffer = NULL;
	DWORD nbRead = 0;

	Stream_Read_UINT32(irp->input, Length); /* Length (4 bytes) */
	Stream_Read_UINT64(irp->input, Offset); /* Offset (8 bytes) */
	Stream_Seek(irp->input, 20); /* Padding (20 bytes) */


	buffer = (BYTE*)calloc(Length, sizeof(BYTE));
	if (buffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}

	
	/* MS-RDPESP 3.2.5.1.4: If the Offset field is not set to 0, the value MUST be ignored
	 * assert(Offset == 0);
	 */
	

	DEBUG_SVC("reading %lu bytes from %s", Length, serial->device.name);

	/* FIXME: CommReadFile to be replaced by ReadFile */
	if (CommReadFile(serial->hComm, buffer, Length, &nbRead, NULL))
	{
		irp->IoStatus = STATUS_SUCCESS;
	}
	else
	{
		DEBUG_SVC("read failure to %s, nbRead=%d, last-error: 0x%0.8x", serial->device.name, nbRead, GetLastError());

		switch(GetLastError())
		{
			case ERROR_INVALID_HANDLE:
				irp->IoStatus = STATUS_INVALID_DEVICE_REQUEST;
				break;

			case ERROR_NOT_SUPPORTED:
				irp->IoStatus = STATUS_NOT_SUPPORTED;
				break;
				
			case ERROR_INVALID_PARAMETER:
				irp->IoStatus = STATUS_INVALID_PARAMETER;
				break;

			case ERROR_IO_DEVICE:
				irp->IoStatus = STATUS_IO_DEVICE_ERROR;
				break;

			case ERROR_TIMEOUT:
				irp->IoStatus = STATUS_TIMEOUT;
				break;

			case ERROR_BAD_DEVICE:
				irp->IoStatus = STATUS_INVALID_DEVICE_REQUEST;
				break;

			case ERROR_CANCELLED:
				irp->IoStatus = STATUS_CANCELLED;
				break;

			default:
				DEBUG_SVC("unexpected last-error: 0x%x", GetLastError());
				irp->IoStatus = STATUS_UNSUCCESSFUL;
				break;
		}
	}

	DEBUG_SVC("%lu bytes read from %s", nbRead, serial->device.name);
	
  error_handle:

	Stream_Write_UINT32(irp->output, nbRead); /* Length (4 bytes) */

	if (nbRead > 0)
	{
		Stream_EnsureRemainingCapacity(irp->output, nbRead);
		Stream_Write(irp->output, buffer, nbRead); /* ReadData */
	}

	if (buffer)
		free(buffer);
}

static void serial_process_irp_write(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 Length;
	UINT64 Offset;
	DWORD nbWritten = 0;

	Stream_Read_UINT32(irp->input, Length); /* Length (4 bytes) */
	Stream_Read_UINT64(irp->input, Offset); /* Offset (8 bytes) */
	Stream_Seek(irp->input, 20); /* Padding (20 bytes) */

	/* MS-RDPESP 3.2.5.1.5: The Offset field is ignored
	 * assert(Offset == 0);
	 *
	 * Using a serial printer, noticed though this field could be
	 * set.
	 */

	DEBUG_SVC("writing %lu bytes to %s", Length, serial->device.name);

	/* FIXME: CommWriteFile to be replaced by WriteFile */
	if (CommWriteFile(serial->hComm, Stream_Pointer(irp->input), Length, &nbWritten, NULL))
	{
		irp->IoStatus = STATUS_SUCCESS;
	}
	else
	{
		DEBUG_SVC("write failure to %s, nbWritten=%d, last-error: 0x%0.8x", serial->device.name, nbWritten, GetLastError());
		switch(GetLastError())
		{
			case ERROR_INVALID_HANDLE:
				irp->IoStatus = STATUS_INVALID_DEVICE_REQUEST;
				break;

			case ERROR_NOT_SUPPORTED:
				irp->IoStatus = STATUS_NOT_SUPPORTED;
				break;
				
			case ERROR_INVALID_PARAMETER:
				irp->IoStatus = STATUS_INVALID_PARAMETER;
				break;

			case ERROR_IO_DEVICE:
				irp->IoStatus = STATUS_IO_DEVICE_ERROR;
				break;

			case ERROR_TIMEOUT:
				irp->IoStatus = STATUS_TIMEOUT;
				break;

			case ERROR_BAD_DEVICE:
				irp->IoStatus = STATUS_INVALID_DEVICE_REQUEST;
				break;

			case ERROR_CANCELLED:
				irp->IoStatus = STATUS_CANCELLED;
				break;

			default:
				DEBUG_SVC("unexpected last-error: 0x%x", GetLastError());
				irp->IoStatus = STATUS_UNSUCCESSFUL;
				break;
		}
	}

	DEBUG_SVC("%lu bytes written to %s", nbWritten, serial->device.name);

	Stream_Write_UINT32(irp->output, nbWritten); /* Length (4 bytes) */
	Stream_Write_UINT8(irp->output, 0); /* Padding (1 byte) */
}


static void serial_process_irp_device_control(SERIAL_DEVICE* serial, IRP* irp)
{
	UINT32 IoControlCode;
	UINT32 InputBufferLength;
	BYTE*  InputBuffer = NULL;
	UINT32 OutputBufferLength;
	BYTE*  OutputBuffer = NULL;
	DWORD  BytesReturned = 0;

	Stream_Read_UINT32(irp->input, OutputBufferLength); /* OutputBufferLength (4 bytes) */
	Stream_Read_UINT32(irp->input, InputBufferLength); /* InputBufferLength (4 bytes) */
	Stream_Read_UINT32(irp->input, IoControlCode); /* IoControlCode (4 bytes) */
	Stream_Seek(irp->input, 20); /* Padding (20 bytes) */

	OutputBuffer = (BYTE*)calloc(OutputBufferLength, sizeof(BYTE));
	if (OutputBuffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}

	InputBuffer = (BYTE*)calloc(InputBufferLength, sizeof(BYTE));
	if (InputBuffer == NULL)
	{
		irp->IoStatus = STATUS_NO_MEMORY;
		goto error_handle;
	}

	Stream_Read(irp->input, InputBuffer, InputBufferLength);

	DEBUG_SVC("CommDeviceIoControl: CompletionId=%d, IoControlCode=[0x%X] %s", irp->CompletionId, IoControlCode, _comm_serial_ioctl_name(IoControlCode));
		
	/* FIXME: CommDeviceIoControl to be replaced by DeviceIoControl() */
	if (CommDeviceIoControl(serial->hComm, IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength, &BytesReturned, NULL))
	{
		/* DEBUG_SVC("CommDeviceIoControl: CompletionId=%d, IoControlCode=[0x%X] %s done", irp->CompletionId, IoControlCode, _comm_serial_ioctl_name(IoControlCode)); */

		irp->IoStatus = STATUS_SUCCESS;
	}
	else
	{
		DEBUG_SVC("CommDeviceIoControl failure: IoControlCode=[0x%0.8x] %s, last-error: 0x%x", 
			  IoControlCode, _comm_serial_ioctl_name(IoControlCode), GetLastError());

		// TMP: TODO: Status codes to be reviewed according: http://msdn.microsoft.com/en-us/library/ff547466%28v=vs.85%29.aspx#generic_status_values_for_serial_device_control_requests

		switch(GetLastError())
		{
			case ERROR_INVALID_HANDLE:
				irp->IoStatus = STATUS_INVALID_DEVICE_REQUEST;
				break;

			case ERROR_NOT_SUPPORTED:
				irp->IoStatus = STATUS_NOT_SUPPORTED;
				break;

			case ERROR_INSUFFICIENT_BUFFER:
				irp->IoStatus = STATUS_BUFFER_TOO_SMALL; /* TMP: better have STATUS_BUFFER_SIZE_TOO_SMALL? http://msdn.microsoft.com/en-us/library/windows/hardware/ff547466%28v=vs.85%29.aspx#generic_status_values_for_serial_device_control_requests */
				break;

			case ERROR_INVALID_PARAMETER:
				irp->IoStatus = STATUS_INVALID_PARAMETER;
				break;

			case ERROR_CALL_NOT_IMPLEMENTED:
				irp->IoStatus = STATUS_NOT_IMPLEMENTED;
				break;

			case ERROR_IO_PENDING:
				irp->IoStatus = STATUS_PENDING;
				break;

			case ERROR_INVALID_DEVICE_OBJECT_PARAMETER: /* eg: SerCx2.sys' _purge() */
				irp->IoStatus = STATUS_INVALID_DEVICE_STATE;
				break;

			case ERROR_CANCELLED:
				irp->IoStatus = STATUS_CANCELLED;
				break;

			default:
				DEBUG_SVC("unexpected last-error: 0x%x", GetLastError());
				irp->IoStatus = STATUS_UNSUCCESSFUL;
				break;
		}
	}

  error_handle:

	assert(OutputBufferLength == BytesReturned);

	Stream_Write_UINT32(irp->output, BytesReturned); /* OutputBufferLength (4 bytes) */

	if (BytesReturned > 0)
	{
		Stream_EnsureRemainingCapacity(irp->output, BytesReturned);
		Stream_Write(irp->output, OutputBuffer, BytesReturned); /* OutputBuffer */
	}
	/* FIXME: Why at least Windows 2008R2 gets lost with this
	 * extra byte and likely on a IOCTL_SERIAL_SET_BAUD_RATE? The
	 * extra byte is well required according MS-RDPEFS
	 * 2.2.1.5.5 */
	/* else */
	/* { */
	/* 	Stream_Write_UINT8(irp->output, 0); /\* Padding (1 byte) *\/ */
	/* } */

	if (InputBuffer != NULL)
		free(InputBuffer);

	if (OutputBuffer != NULL)
		free(OutputBuffer);
}

static void serial_process_irp(SERIAL_DEVICE* serial, IRP* irp)
{
	WLog_Print(serial->log, WLOG_DEBUG, "IRP MajorFunction: 0x%04X MinorFunction: 0x%04X\n",
			irp->MajorFunction, irp->MinorFunction);

	switch (irp->MajorFunction)
	{
		case IRP_MJ_CREATE:
			serial_process_irp_create(serial, irp);
			break;

		case IRP_MJ_CLOSE:
			serial_process_irp_close(serial, irp);
			break;

		case IRP_MJ_READ:
			serial_process_irp_read(serial, irp);
			break;

		case IRP_MJ_WRITE:
			serial_process_irp_write(serial, irp);
			break;

		case IRP_MJ_DEVICE_CONTROL:
			serial_process_irp_device_control(serial, irp);
			break;

		default:
			DEBUG_WARN("MajorFunction 0x%X not supported", irp->MajorFunction);
			irp->IoStatus = STATUS_NOT_SUPPORTED;
			break;
	}
}


static void* irp_thread_func(void* arg)
{
	IRP_THREAD_DATA *data = (IRP_THREAD_DATA*)arg;

	/* blocks until the end of the request */
	serial_process_irp(data->serial, data->irp);

	EnterCriticalSection(&data->serial->TerminatingIrpThreadsLock);
	data->serial->IrpThreadToBeTerminatedCount++;

	data->irp->Complete(data->irp);

	LeaveCriticalSection(&data->serial->TerminatingIrpThreadsLock);

	/* NB: At this point, the server might already being reusing
	 * the CompletionId whereas the thread is not yet
	 * terminated */

	free(data);

	ExitThread(0);
	return NULL;
}


static void create_irp_thread(SERIAL_DEVICE *serial, IRP *irp)
{
	IRP_THREAD_DATA *data = NULL;
	HANDLE irpThread = INVALID_HANDLE_VALUE;
	HANDLE previousIrpThread;

	/* for a test/debug purpose, uncomment the code below to get a
	 * single thread for all IRPs. NB: two IRPs could not be
	 * processed at the same time, typically two concurent
	 * Read/Write operations could block each other. */
	/* serial_process_irp(serial, irp); */
	/* irp->Complete(irp); */
	/* return; */


	/* NOTE: for good or bad, this implementation relies on the
	 * server to avoid a flooding of requests. see also _purge().
	 */

	EnterCriticalSection(&serial->TerminatingIrpThreadsLock);
	while (serial->IrpThreadToBeTerminatedCount > 0)
	{
		/* Cleaning up termitating and pending irp
		 * threads. See also: irp_thread_func() */

		HANDLE irpThread;
		ULONG_PTR *ids;
		int i, nbIds;

		nbIds = ListDictionary_GetKeys(serial->IrpThreads, &ids);
		for (i=0; i<nbIds; i++)
		{
			/* Checking if ids[i] is terminating or pending */

			DWORD waitResult;
			ULONG_PTR id = ids[i];

			irpThread = ListDictionary_GetItemValue(serial->IrpThreads, (void*)id);

			/* FIXME: not quite sure a zero timeout is a good thing to check whether a thread is stil alived or not */
			waitResult = WaitForSingleObject(irpThread, 0);
			if (waitResult == WAIT_OBJECT_0)
			{
				/* terminating thread */

				/* DEBUG_SVC("IRP thread with CompletionId=%d naturally died", id); */

				CloseHandle(irpThread);
				ListDictionary_Remove(serial->IrpThreads, (void*)id);

				serial->IrpThreadToBeTerminatedCount--;
			}
			else if (waitResult != WAIT_TIMEOUT)
 			{
				/* unexpected thread state */

				DEBUG_WARN("WaitForSingleObject, got an unexpected result=0x%X\n", waitResult);
				assert(FALSE);
			}
			/* pending thread (but not yet terminating thread) if waitResult == WAIT_TIMEOUT */
		}
		

		if (serial->IrpThreadToBeTerminatedCount > 0)
		{
			DEBUG_SVC("%d IRP thread(s) not yet terminated", serial->IrpThreadToBeTerminatedCount);
			Sleep(1); /* 1 ms */
		}
	}
	LeaveCriticalSection(&serial->TerminatingIrpThreadsLock);

	/* NB: At this point and thanks to the synchronization we're
	 * sure that the incoming IRP uses well a recycled
	 * CompletionId or the server sent again an IRP already posted
	 * which didn't get yet a response (this later server behavior
	 * at least observed with IOCTL_SERIAL_WAIT_ON_MASK and
	 * mstsc.exe.
	 *
	 * FIXME: behavior documented somewhere? behavior not yet
	 * observed with FreeRDP).
	 */

	previousIrpThread = ListDictionary_GetItemValue(serial->IrpThreads, (void*)irp->CompletionId);
	if (previousIrpThread) 
	{
		/* Thread still alived <=> Request still pending */

		DEBUG_SVC("IRP recall: IRP with the CompletionId=%d not yet completed!", irp->CompletionId);

		assert(FALSE); /* unimplemented */

		/* TODO: asserts that previousIrpThread handles well
		 * the same request by checking more details. Need an
		 * access to the IRP object used by previousIrpThread
		 */

		/* TODO: taking over the pending IRP or sending a kind
		 * of wake up signal to accelerate the pending
		 * request
		 *
		 * To be considered:
		 *   if (IoControlCode == IOCTL_SERIAL_WAIT_ON_MASK) {
		 *       pComm->PendingEvents |= SERIAL_EV_FREERDP_*;
		 *   }
		 */

		irp->Discard(irp);
		return;
	}


	if (ListDictionary_Count(serial->IrpThreads) >= MAX_IRP_THREADS)
	{
		DEBUG_WARN("Number of IRP threads threshold reached: %d, keep on anyway", ListDictionary_Count(serial->IrpThreads));
		
		assert(FALSE); /* unimplemented */

		/* TODO: MAX_IRP_THREADS has been thought to avoid a
		 * flooding of pending requests. Use
		 * WaitForMultipleObjects() when available in winpr
		 * for threads.
		 */
	}


	/* error_handle to be used ... */

	data = (IRP_THREAD_DATA*)calloc(1, sizeof(IRP_THREAD_DATA));
	if (data == NULL)
	{
		DEBUG_WARN("Could not allocate a new IRP_THREAD_DATA.");
		goto error_handle;
	}

	data->serial = serial;
	data->irp = irp;

	/* data freed by irp_thread_func */

	irpThread = CreateThread(NULL, 
				 0, 
				 (LPTHREAD_START_ROUTINE)irp_thread_func, 
				 (void*)data, 
				 0, 
				 NULL);

	if (irpThread == INVALID_HANDLE_VALUE)
	{
		DEBUG_WARN("Could not allocate a new IRP thread.");
		goto error_handle;
	}



	ListDictionary_Add(serial->IrpThreads, (void*)irp->CompletionId, irpThread);

	return;

  error_handle:

	irp->IoStatus = STATUS_NO_MEMORY;
	irp->Complete(irp);

	if (data)
		free(data);
}


static void terminate_pending_irp_threads(SERIAL_DEVICE *serial)
{
	ULONG_PTR *ids;
	int i, nbIds;

	nbIds = ListDictionary_GetKeys(serial->IrpThreads, &ids);

	DEBUG_SVC("Terminating %d IRP thread(s)", nbIds);

	for (i=0; i<nbIds; i++)
	{
		HANDLE irpThread;
		ULONG_PTR id = ids[i];

		irpThread = ListDictionary_GetItemValue(serial->IrpThreads, (void*)id);		

		TerminateThread(irpThread, 0);

		WaitForSingleObject(irpThread, INFINITE);

		CloseHandle(irpThread);

		DEBUG_SVC("IRP thread terminated, CompletionId %d", id);
	}

	ListDictionary_Clear(serial->IrpThreads);
}


static void* serial_thread_func(void* arg)
{
	IRP* irp;
	wMessage message;
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*) arg;

	while (1)
	{
		if (!MessageQueue_Wait(serial->MainIrpQueue))
			break;

		if (!MessageQueue_Peek(serial->MainIrpQueue, &message, TRUE))
			break;

		if (message.id == WMQ_QUIT)
		{
			terminate_pending_irp_threads(serial);
			break;
		}

		irp = (IRP*) message.wParam;

		if (irp)
			create_irp_thread(serial, irp);
	}

	ExitThread(0);
	return NULL;
}


static void serial_irp_request(DEVICE* device, IRP* irp)
{
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*) device;

	assert(irp != NULL);

	if (irp == NULL)
		return;

	/* NB: ENABLE_ASYNCIO is set, (MS-RDPEFS 2.2.2.7.2) this
	 * allows the server to send multiple simultaneous read or
	 * write requests.
	 */

	MessageQueue_Post(serial->MainIrpQueue, NULL, 0, (void*) irp, NULL);
}

static void serial_free(DEVICE* device)
{
	SERIAL_DEVICE* serial = (SERIAL_DEVICE*) device;
	
	WLog_Print(serial->log, WLOG_DEBUG, "freeing");

	MessageQueue_PostQuit(serial->MainIrpQueue, 0);
	WaitForSingleObject(serial->MainThread, INFINITE);
	CloseHandle(serial->MainThread);

	if (serial->hComm)
		CloseHandle(serial->hComm);

	/* Clean up resources */
	Stream_Free(serial->device.data, TRUE);
	MessageQueue_Free(serial->MainIrpQueue);
	ListDictionary_Free(serial->IrpThreads);
	DeleteCriticalSection(&serial->TerminatingIrpThreadsLock);

	free(serial);
}

#ifdef STATIC_CHANNELS
#define DeviceServiceEntry	serial_DeviceServiceEntry
#endif

int DeviceServiceEntry(PDEVICE_SERVICE_ENTRY_POINTS pEntryPoints)
{
	int i, len;
	char* name;
	char* path;
	RDPDR_SERIAL* device;
	SERIAL_DEVICE* serial;

	device = (RDPDR_SERIAL*) pEntryPoints->device;
	name = device->Name;
	path = device->Path;

	if (!name || (name[0] == '*'))
	{
		/* TODO: implement auto detection of serial ports */
		return 0;
	}

	if ((name && name[0]) && (path && path[0]))
	{
		DEBUG_SVC("Defining %s as %s", name, path);
			
		if (!DefineCommDevice(name /* eg: COM1 */, path /* eg: /dev/ttyS0 */))
		{
			DEBUG_SVC("Could not define %s as %s", name, path);
			return -1;
		}

		serial = (SERIAL_DEVICE*) calloc(1, sizeof(SERIAL_DEVICE));
		if (!serial)
			return -1;

		serial->device.type = RDPDR_DTYP_SERIAL;
		serial->device.name = name;
		serial->device.IRPRequest = serial_irp_request;
		serial->device.Free = serial_free;

		len = strlen(name);
		serial->device.data = Stream_New(NULL, len + 1);

		for (i = 0; i <= len; i++)
			Stream_Write_UINT8(serial->device.data, name[i] < 0 ? '_' : name[i]);

		serial->MainIrpQueue = MessageQueue_New(NULL);

		/* IrpThreads content only modified by create_irp_thread() */
		serial->IrpThreads = ListDictionary_New(FALSE); 
		serial->IrpThreadToBeTerminatedCount = 0;
		InitializeCriticalSection(&serial->TerminatingIrpThreadsLock);

		WLog_Init();
		serial->log = WLog_Get("com.freerdp.channel.serial.client");
		WLog_Print(serial->log, WLOG_DEBUG, "initializing");

		pEntryPoints->RegisterDevice(pEntryPoints->devman, (DEVICE*) serial);

		serial->MainThread = CreateThread(NULL, 
						  0, 
						  (LPTHREAD_START_ROUTINE) serial_thread_func, 
						  (void*) serial, 
						  0, 
						  NULL);
	}

	return 0;
}

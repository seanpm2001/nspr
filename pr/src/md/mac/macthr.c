/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* 
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is the Netscape Portable Runtime (NSPR).
 * 
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are 
 * Copyright (C) 1998-2000 Netscape Communications Corporation.  All
 * Rights Reserved.
 * 
 * Contributor(s):
 * 
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU General Public License Version 2 or later (the
 * "GPL"), in which case the provisions of the GPL are applicable 
 * instead of those above.  If you wish to allow use of your 
 * version of this file only under the terms of the GPL and not to
 * allow others to use your version of this file under the MPL,
 * indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by
 * the GPL.  If you do not delete the provisions above, a recipient
 * may use your version of this file under either the MPL or the
 * GPL.
 */

#include "primpl.h"

#include <string.h>

#include <Types.h>
#include <Timer.h>
#include <OSUtils.h>

#include <LowMem.h>


TimerUPP	gTimerCallbackUPP	= NULL;
PRThread *	gPrimaryThread		= NULL;

PR_IMPLEMENT(PRThread *) PR_GetPrimaryThread()
{
	return gPrimaryThread;
}

//##############################################################################
//##############################################################################
#pragma mark -
#pragma mark CREATING MACINTOSH THREAD STACKS

#if defined(GC_LEAK_DETECTOR)
extern void* GC_malloc_atomic(PRUint32 size);
#endif

/*
**	Allocate a new memory segment.  We allocate it from our figment heap.  Currently,
**	it is being used for per thread stack space.
**	
**	Return the segment's access rights and size.  vaddr is used on Unix platforms to
**	map an existing address for the segment.
*/
PRStatus _MD_AllocSegment(PRSegment *seg, PRUint32 size, void *vaddr)
{
	PR_ASSERT(seg != 0);
	PR_ASSERT(size != 0);
	PR_ASSERT(vaddr == 0);

	/*	
	** Take the actual memory for the segment out of our Figment heap.
	*/

#if defined(GC_LEAK_DETECTOR)
	seg->vaddr = (char *)GC_malloc_atomic(size);
#else
	seg->vaddr = (char *)malloc(size);
#endif

	if (seg->vaddr == NULL) {

#if DEBUG
		DebugStr("\p_MD_AllocSegment failed.");
#endif

		return PR_FAILURE;
	}

	seg->size = size;	

	return PR_SUCCESS;
}


/*
**	Free previously allocated memory segment.
*/
void _MD_FreeSegment(PRSegment *seg)
{
	PR_ASSERT((seg->flags & _PR_SEG_VM) == 0);

	if (seg->vaddr != NULL)
		free(seg->vaddr);
}


/*
**	The thread's stack has been allocated and its fields are already properly filled
**	in by PR.  Perform any debugging related initialization here.
**
**	Put a recognizable pattern so that we can find it from Macsbug.
**	Put a cookie at the top of the stack so that we can find it from Macsbug.
*/
void _MD_InitStack(PRThreadStack *ts, int redZoneBytes)
	{
#pragma unused (redZoneBytes)
#if DEVELOPER_DEBUG
	//	Put a cookie at the top of the stack so that we can find 
	//	it from Macsbug.
	
	memset(ts->allocBase, 0xDC, ts->stackSize);
	
	((UInt32 *)ts->stackTop)[-1] = 0xBEEFCAFE;
	((UInt32 *)ts->stackTop)[-2] = (UInt32)gPrimaryThread;
	((UInt32 *)ts->stackTop)[-3] = (UInt32)(ts);
	((UInt32 *)ts->stackBottom)[0] = 0xCAFEBEEF;
#else
#pragma unused (ts)
#endif	
	}

extern void _MD_ClearStack(PRThreadStack *ts)
	{
#if DEVELOPER_DEBUG
	//	Clear out our cookies. 
	
	memset(ts->allocBase, 0xEF, ts->allocSize);
	((UInt32 *)ts->stackTop)[-1] = 0;
	((UInt32 *)ts->stackTop)[-2] = 0;
	((UInt32 *)ts->stackTop)[-3] = 0;
	((UInt32 *)ts->stackBottom)[0] = 0;
#else
#pragma unused (ts)
#endif
	}


//##############################################################################
//##############################################################################
#pragma mark -
#pragma mark TIME MANAGER-BASED CLOCK

TMTask		gTimeManagerTaskElem;

extern void _MD_IOInterrupt(void);
_PRInterruptTable _pr_interruptTable[] = {
    { "clock", _PR_MISSED_CLOCK, _PR_ClockInterrupt, },
    { "i/o", _PR_MISSED_IO, _MD_IOInterrupt, },
    { 0 }
};

pascal void TimerCallback(TMTaskPtr tmTaskPtr)
{
    _PRCPU *cpu = _PR_MD_CURRENT_CPU();

    if (_PR_MD_GET_INTSOFF()) {
        cpu->u.missed[cpu->where] |= _PR_MISSED_CLOCK;
		PrimeTime((QElemPtr)tmTaskPtr, kMacTimerInMiliSecs);
		return;
    }
    _PR_MD_SET_INTSOFF(1);

	//	And tell nspr that a clock interrupt occured.
	_PR_ClockInterrupt();
	
	if ((_PR_RUNQREADYMASK(cpu)) >> ((_PR_MD_CURRENT_THREAD()->priority)))
		_PR_SET_RESCHED_FLAG();
	
    _PR_MD_SET_INTSOFF(0);

	//	Reset the clock timer so that we fire again.
	PrimeTime((QElemPtr)tmTaskPtr, kMacTimerInMiliSecs);
}


void _MD_StartInterrupts(void)
{
	gPrimaryThread = _PR_MD_CURRENT_THREAD();

	if ( !gTimerCallbackUPP )
		gTimerCallbackUPP = NewTimerProc(TimerCallback);

	//	Fill in the Time Manager queue element
	
	gTimeManagerTaskElem.tmAddr = (TimerUPP)gTimerCallbackUPP;
	gTimeManagerTaskElem.tmCount = 0;
	gTimeManagerTaskElem.tmWakeUp = 0;
	gTimeManagerTaskElem.tmReserved = 0;

	//	Make sure that our time manager task is ready to go.
	InsTime((QElemPtr)&gTimeManagerTaskElem);
	
	PrimeTime((QElemPtr)&gTimeManagerTaskElem, kMacTimerInMiliSecs);
}

void _MD_StopInterrupts(void)
{
	if (gTimeManagerTaskElem.tmAddr != NULL) {
		RmvTime((QElemPtr)&gTimeManagerTaskElem);
		gTimeManagerTaskElem.tmAddr = NULL;
	}
}

void _MD_PauseCPU(PRIntervalTime timeout)
{
#pragma unused (timeout)

	/* unsigned long finalTicks; */
	EventRecord theEvent;
	
	if (timeout != PR_INTERVAL_NO_WAIT) {
	   /* Delay(1,&finalTicks); */
	   
	   /*
	   ** Rather than calling Delay() which basically just wedges the processor
	   ** we'll instead call WaitNextEvent() with a mask that ignores all events
	   ** which gives other apps a chance to get time rather than just locking up
	   ** the machine when we're waiting for a long time (or in an infinite loop,
	   ** whichever comes first)
	   */
	   (void)WaitNextEvent(nullEvent, &theEvent, 1, NULL);
	   
	    (void) _MD_IOInterrupt();
	}
}


//##############################################################################
//##############################################################################
#pragma mark -
#pragma mark THREAD SUPPORT FUNCTIONS

#include <OpenTransport.h> /* for error codes */

PRStatus _MD_InitThread(PRThread *thread)
{
	thread->md.asyncIOLock = PR_NewLock();
	PR_ASSERT(thread->md.asyncIOLock != NULL);
	thread->md.asyncIOCVar = PR_NewCondVar(thread->md.asyncIOLock);
	PR_ASSERT(thread->md.asyncIOCVar != NULL);

	if (thread->md.asyncIOLock == NULL || thread->md.asyncIOCVar == NULL)
		return PR_FAILURE;
	else
		return PR_SUCCESS;
}

PRStatus _MD_wait(PRThread *thread, PRIntervalTime timeout)
{
#pragma unused (timeout)

	_MD_SWITCH_CONTEXT(thread);
	return PR_SUCCESS;
}


void WaitOnThisThread(PRThread *thread, PRIntervalTime timeout)
{
    intn is;
    PRIntervalTime timein = PR_IntervalNow();
	PRStatus status = PR_SUCCESS;

	_PR_INTSOFF(is);
	PR_Lock(thread->md.asyncIOLock);
	if (timeout == PR_INTERVAL_NO_TIMEOUT) {
	    while ((thread->io_pending) && (status == PR_SUCCESS))
	        status = PR_WaitCondVar(thread->md.asyncIOCVar, PR_INTERVAL_NO_TIMEOUT);
	} else {
	    while ((thread->io_pending) && ((PRIntervalTime)(PR_IntervalNow() - timein) < timeout) && (status == PR_SUCCESS))
	        status = PR_WaitCondVar(thread->md.asyncIOCVar, timeout);
	}
	if ((status == PR_FAILURE) && (PR_GetError() == PR_PENDING_INTERRUPT_ERROR)) {
		thread->md.osErrCode = kEINTRErr;
	} else if (thread->io_pending) {
		thread->md.osErrCode = kETIMEDOUTErr;
		PR_SetError(PR_IO_TIMEOUT_ERROR, kETIMEDOUTErr);
	}
	PR_Unlock(thread->md.asyncIOLock);
	_PR_FAST_INTSON(is);
}


void DoneWaitingOnThisThread(PRThread *thread)
{
    intn is;

	_PR_INTSOFF(is);
	PR_Lock(thread->md.asyncIOLock);
    thread->io_pending = PR_FALSE;
	/* let the waiting thread know that async IO completed */
	PR_NotifyCondVar(thread->md.asyncIOCVar);
	PR_Unlock(thread->md.asyncIOLock);
	_PR_FAST_INTSON(is);
}


PR_IMPLEMENT(void) PR_Mac_WaitForAsyncNotify(PRIntervalTime timeout)
{
    intn is;
    PRIntervalTime timein = PR_IntervalNow();
	PRStatus status = PR_SUCCESS;
    PRThread *thread = _PR_MD_CURRENT_THREAD();

	_PR_INTSOFF(is);
	PR_Lock(thread->md.asyncIOLock);
	if (timeout == PR_INTERVAL_NO_TIMEOUT) {
	    while ((!thread->md.asyncNotifyPending) && (status == PR_SUCCESS))
	        status = PR_WaitCondVar(thread->md.asyncIOCVar, PR_INTERVAL_NO_TIMEOUT);
	} else {
	    while ((!thread->md.asyncNotifyPending) && ((PRIntervalTime)(PR_IntervalNow() - timein) < timeout) && (status == PR_SUCCESS))
	        status = PR_WaitCondVar(thread->md.asyncIOCVar, timeout);
	}
	if ((status == PR_FAILURE) && (PR_GetError() == PR_PENDING_INTERRUPT_ERROR)) {
		thread->md.osErrCode = kEINTRErr;
	} else if (!thread->md.asyncNotifyPending) {
		thread->md.osErrCode = kETIMEDOUTErr;
		PR_SetError(PR_IO_TIMEOUT_ERROR, kETIMEDOUTErr);
	}
	thread->md.asyncNotifyPending = PR_FALSE;
	PR_Unlock(thread->md.asyncIOLock);
	_PR_FAST_INTSON(is);
}


void AsyncNotify(PRThread *thread)
{
    intn is;
	
	_PR_INTSOFF(is);
	PR_Lock(thread->md.asyncIOLock);
    thread->md.asyncNotifyPending = PR_TRUE;
	/* let the waiting thread know that async IO completed */
	PR_NotifyCondVar(thread->md.asyncIOCVar);	// let thread know that async IO completed
	PR_Unlock(thread->md.asyncIOLock);
	_PR_FAST_INTSON(is);
}


PR_IMPLEMENT(void) PR_Mac_PostAsyncNotify(PRThread *thread)
{
	_PRCPU *  cpu = _PR_MD_CURRENT_CPU();
	
	if (_PR_MD_GET_INTSOFF()) {
		cpu->u.missed[cpu->where] |= _PR_MISSED_IO;
		thread->md.missedAsyncNotify = PR_TRUE;
	} else {
		AsyncNotify(thread);
	}
}


//##############################################################################
//##############################################################################
#pragma mark -
#pragma mark PROCESS SUPPORT FUNCTIONS

PRProcess * _MD_CreateProcess(
    const char *path,
    char *const *argv,
    char *const *envp,
    const PRProcessAttr *attr)
{
#pragma unused (path, argv, envp, attr)

	PR_SetError(PR_NOT_IMPLEMENTED_ERROR, unimpErr);
	return NULL;
}

PRStatus _MD_DetachProcess(PRProcess *process)
{
#pragma unused (process)

	PR_SetError(PR_NOT_IMPLEMENTED_ERROR, unimpErr);
	return PR_FAILURE;
}

PRStatus _MD_WaitProcess(PRProcess *process, PRInt32 *exitCode)
{
#pragma unused (process, exitCode)

	PR_SetError(PR_NOT_IMPLEMENTED_ERROR, unimpErr);
	return PR_FAILURE;
}

PRStatus _MD_KillProcess(PRProcess *process)
{
#pragma unused (process)

	PR_SetError(PR_NOT_IMPLEMENTED_ERROR, unimpErr);
	return PR_FAILURE;
}

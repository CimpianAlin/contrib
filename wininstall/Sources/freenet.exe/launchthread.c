/*		launchthread.c
		Dave Hooper (@1 May 2001)

		This thread handles actually loading and running the freenet node.
		As it is a separate thread the tray icon never fails to respond to user input
		Also it's better this way as you can get proper notification (through tooltips)
		of what the freenet node is actually doing at any one time */

#include "windows.h"
#include "types.h"
#include "launchthread.h"
#include "shared_data.h"

/******************************************************
 *  G L O B A L S                                     *
 ******************************************************
 *	Global Const data:                                *
 *		null-terminated strings, etc ...              *
 ******************************************************/

const char szerrMsg[]=	"Couldn't start the node,\n"
						"make sure FLaunch.ini has an entry javaw= pointing to javaw.exe or\n"
						"an entry Javaexec= pointing to a Java Runtime binary (jview.exe/java.exe)";
const char szerrTitle[]="Error starting node";



DWORD WINAPI _stdcall MonitorThread(LPVOID null)
{
	MSG msg;
	DWORD dwWait;
	bool bQuitThreadNow; 
	HANDLE hAlwaysUnsignalled;
	HANDLE phobjectlist[2] = {NULL,NULL};
	DWORD dwTimeout;


	/*	create a thread message queue - used to control communication between this
		thread and the main (message pump / WndProc) thread */
	PeekMessage(&msg, (HWND)(-1), 0, 0, PM_NOREMOVE);

	/*	following flag is set to true by the WM_QUITMONITORINGTHREAD
		message handler below in this function */
	bQuitThreadNow=false;

	/* paradigm - an object that is always unsignalled - for use with "MsgWait..." functions */
	hAlwaysUnsignalled = CreateEvent(NULL, TRUE, FALSE, NULL);
	
	
	/* main thread loop - repeat until instructed to quit */
	while (!bQuitThreadNow)
	{

		switch (nFreenetMode)
		{
		case FREENET_RUNNING:
			// we're supposed to be 'monitoring' the thread and/or flashing the warning icon
			// wait for either a posted thread message, or the prcInfo.hProcess object being signalled,
			// or a timeout of 500 milliseconds i.e. half a second
			// Fserve thread is allegedly running - check that it still is!
			// (wait until either Fserve dies or WE receive a thread message telling us to do something different)
			phobjectlist[0] = prcInfo.hProcess;
			dwTimeout = INFINITE;
			break;
		
		case FREENET_CANNOT_START:
			// Fserve failed to load and run - 
			// so just wait for a threadmessage or a 500ms timeout
			// This generates our timeout-driven flashing icon
			phobjectlist[0] = hAlwaysUnsignalled;
			dwTimeout = 500;
			break;

		case FREENET_STOPPING:
		case FREENET_RESTARTING: /* just another kind of 'stopping' */
		case FREENET_STOPPED:
		default:
			// Freenet is either not running, or shutting down
			// so just wait for a threadmessage - no monitoring ... no flashing icon ...
			phobjectlist[0] = hAlwaysUnsignalled;
			dwTimeout = INFINITE;
			break;

		}

		dwWait = MsgWaitForMultipleObjects(1,phobjectlist,FALSE, dwTimeout, QS_POSTMESSAGE);
		switch (dwWait)
		{
			case 0xffffffff:
				// This means MsgWait... had an error ... better give up and die
				break;

			case WAIT_OBJECT_0+1:
				// we have received a posted thread message : deal with it
				while ( PeekMessage(&msg,(HWND)(-1),0,0,PM_REMOVE) )
				{
					switch (msg.message)
					{
					case WM_BEGINMONITORING:
						/* fire up the node! */
						MonitorThreadRunFserve();
						break;

					case WM_ENDMONITORING:
						MonitorThreadKillFserve();
						break;

					case WM_QUITMONITORINGTHREAD:
						bQuitThreadNow=true;
						break;

					case WM_TESTMESSAGE:
					default:
						// do nothing
						break;
					}


				}
				break;

			case WAIT_OBJECT_0:
				// thread has died - begin flashing icon:
				nFreenetMode=FREENET_CANNOT_START;
				//break;  NO BREAK - FALL THROUGH!

			case WAIT_TIMEOUT:
				if (nFreenetMode==FREENET_CANNOT_START)
				{
					// period timeout fired - change the icon
					ModifyIcon();
				}
				break;
		} // switch

	} // while !bQuitThreadNow

	// when we get here, it's because we got a WM_QUITMONITORINGTHREAD message
	CloseHandle(hAlwaysUnsignalled);

	return 0;
}


/* note - ONLY TO BE CALLED FROM ASYNCHRONOUS THREAD LEVEL - i.e. Monitor Thread */
void MonitorThreadRunFserve()
{
	STARTUPINFO StartInfo={	sizeof(STARTUPINFO),
							NULL,NULL,NULL,
							0,0,0,0,0,0,0,
							STARTF_USESHOWWINDOW,
							SW_HIDE,
							0,NULL,
							NULL,NULL,NULL};
	char szexecbuf[sizeof(szjavawpath)+sizeof(szfservecliexec)+2];

	lstrcpy(szexecbuf, szjavawpath);
	lstrcat(szexecbuf, " ");
	lstrcat(szexecbuf, szfservecliexec); 

	if (!CreateProcess(szjavawpath, (char*)(szexecbuf), NULL, NULL, FALSE, NORMAL_PRIORITY_CLASS, NULL, NULL, &StartInfo, &prcInfo) )
	{
		MessageBox(NULL, szerrMsg, szerrTitle, MB_OK | MB_ICONERROR | MB_TASKMODAL);
		nFreenetMode=FREENET_CANNOT_START;
		ModifyIcon();
	}
	else
	{
		/* 'watch' the process object to check that the java interpreter launched correctly
			and that the freenet node is indeed running */
		WaitForInputIdle(prcInfo.hProcess,INFINITE);
		nFreenetMode=FREENET_RUNNING;
		ModifyIcon();
	}
}

/* note - ONLY TO BE CALLED FROM ASYNCHRONOUS THREAD LEVEL - i.e. Monitor Thread */
void MonitorThreadKillFserve()
{
	if ( (prcInfo.hProcess!=NULL) || (prcInfo.hThread!=NULL) )
	{
		/* set nFreenetMode to FREENET_STOPPING only if we didn't get here because of RestartFserve */
		if (nFreenetMode!=FREENET_RESTARTING)
		{
			nFreenetMode=FREENET_STOPPING;
		}
		ModifyIcon();

		/* best effort at closing down the node: */

		/* get the window handle from the process ID by matching all
		   known windows against it (not really ideal but no alternative) */
		EnumWindows(KillWindowByProcessId, (LPARAM)prcInfo.dwProcessId);

		/* wait for the node to shutdown (five seconds) */
		if (WaitForSingleObject(prcInfo.hProcess,5000) == WAIT_TIMEOUT)
		{
			/* Didn't work, ok, so, try sending a WM_CLOSE message to the
			   thread message queue of the application */
			PostThreadMessage(prcInfo.dwThreadId, WM_CLOSE, 0, 0);
			
			/* wait for the node to shutdown (two-and-a-half seconds) */
			if (WaitForSingleObject(prcInfo.hProcess,2500) == WAIT_TIMEOUT)
			{
				/* OH MY, nothing worked - ok then, brutally close the node: */
				TerminateProcess(prcInfo.hProcess,0);
			}
		}
		CloseHandle(prcInfo.hThread);
		CloseHandle(prcInfo.hProcess);
		prcInfo.hThread=NULL;
		prcInfo.hProcess=NULL;
		prcInfo.dwThreadId=0;
		prcInfo.dwProcessId=0;
	}
	nFreenetMode=FREENET_STOPPED;
	ModifyIcon();
}


/* If this looks confusing see article Q178893 in MSDN.
   All I'm doing is enumerating ALL the top-level windows in the system and matching
   against the Freenet Node process handle.  I send a swift WM_CLOSE to any that match
   and sit back and wait for the results */
BOOL CALLBACK KillWindowByProcessId(HWND hWnd, LPARAM lParam)
{
	/* called for each window in system */
	/* we're using it to hunt for windows created by the freenet node process */
	/* First find out if this window was created by the freenet node process: */
	DWORD dwThreadId;
	GetWindowThreadProcessId(hWnd, &dwThreadId);
	if (dwThreadId != (DWORD)lParam)
	{
		/* This window was NOT created by the process... keep enumerating */
		return TRUE;
	}

	/* This window WAS created by the process - 
	   Post a swift WM_CLOSE message to tell the window to close the underlying app */
	PostMessage(hWnd, WM_CLOSE, 0, 0);

	/* return true to keep enumerating - there may be more windows that need shutting down */
	return TRUE;
}
 

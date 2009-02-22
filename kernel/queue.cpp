/*
 * message queues
 *
 * Based on wine/server/queue.c
 * Copyright (C) 2000 Alexandre Julliard
 *
 * Modifications for ring3k
 * Copyright 2006-2009 Mike McCormack
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#include <stdarg.h>
#include <stdio.h>
#include <assert.h>
#include <new>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "ntcall.h"
#include "object.h"
#include "ntwin32.h"
#include "mem.h"
#include "debug.h"
#include "object.inl"
#include "list.h"
#include "timer.h"
#include "win.h"
#include "queue.h"
#include "spy.h"

msg_tt::msg_tt( HWND _hwnd, UINT _message, WPARAM _wparam, LPARAM _lparam ) :
	hwnd( _hwnd ),
	message( _message ),
	wparam( _wparam ),
	lparam( _lparam )
{
	time = timeout_t::get_tick_count();
}

thread_message_queue_tt::thread_message_queue_tt() :
	quit_message( 0 ),
	exit_code( 0 )
{
}

thread_message_queue_tt::~thread_message_queue_tt()
{
	msg_tt *msg;

	while ((msg = msg_list.head()))
	{
		msg_list.unlink( msg );
		delete msg;
	}
}

bool thread_message_queue_tt::get_quit_message( MSG& msg )
{
	bool ret = quit_message;
	if (quit_message)
	{
		msg.message = WM_QUIT;
		msg.wParam = exit_code;
		quit_message = false;
	}
	return ret;
}

bool thread_message_queue_tt::get_paint_message( HWND Window, MSG& msg )
{
	window_tt *win = window_tt::find_window_to_repaint( Window, current );
	if (!win)
		return FALSE;

	msg.message = WM_PAINT;
	msg.time = timeout_t::get_tick_count();
	msg.hwnd = win->handle;

	return TRUE;
}

BOOLEAN thread_message_queue_tt::is_signalled( void )
{
	return FALSE;
}

void thread_message_queue_tt::post_quit_message( ULONG ret )
{
	quit_message = true;
	exit_code = ret;
}

BOOL thread_message_queue_tt::post_message(
	HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam )
{
	msg_waiter_tt *waiter = waiter_list.head();
	if (waiter)
	{
		MSG& msg = waiter->msg;
		msg.hwnd = Window;
		msg.message = Message;
		msg.wParam = Wparam;
		msg.lParam = Lparam;
		msg.time = timeout_t::get_tick_count();
		msg.pt.x = 0;
		msg.pt.y = 0;

		// remove from the list first
		waiter_list.unlink( waiter );

		// start the thread (might reschedule here )
		waiter->t->start();

		return TRUE;
	}

	// no waiter, so store the message
	msg_tt* msg = new msg_tt( Window, Message, Wparam, Lparam );
	if (!msg)
		return FALSE;
	msg_list.append( msg );

	// FIXME: wake up a thread that is waiting
	return TRUE;
}

// return true if we copied a message
bool thread_message_queue_tt::get_posted_message( HWND Window, MSG& Message )
{
	msg_tt *m = msg_list.head();
	if (!m)
		return false;

	msg_list.unlink( m );
	Message.hwnd = m->hwnd;
	Message.message = m->message;
	Message.wParam = m->wparam;
	Message.lParam = m->lparam;
	Message.time = m->time;
	Message.pt.x = 0;
	Message.pt.y = 0;
	delete m;

	return true;
}

msg_waiter_tt::msg_waiter_tt( MSG& m):
	msg( m )
{
	t = current;
}

BOOLEAN thread_message_queue_tt::set_timer( HWND Window, UINT Identifier, UINT Elapse, PVOID TimerProc )
{
	return FALSE;
}

BOOLEAN thread_message_queue_tt::kill_timer( HWND Window, UINT Identifier )
{
	return FALSE;
}

// return true if we succeeded in copying a message
BOOLEAN thread_message_queue_tt::get_message_no_wait(
	MSG& Message, HWND Window, ULONG MinMessage, ULONG MaxMessage)
{
	if (get_posted_message( Window, Message ))
		return true;

	if (get_quit_message( Message ))
		return true;

	if (get_paint_message( Window, Message ))
		return true;

	return false;
}

BOOLEAN thread_message_queue_tt::get_message(
	MSG& Message, HWND Window, ULONG MinMessage, ULONG MaxMessage)
{
	if (get_message_no_wait( Message, Window, MinMessage, MaxMessage))
		return true;

	// wait for a message
	// a thread sending a message will restart us
	msg_waiter_tt wait( Message );
	waiter_list.append( &wait );
	current->stop();

	return !current->is_terminated();
}

BOOLEAN NTAPI NtUserGetMessage(PMSG Message, HWND Window, ULONG MinMessage, ULONG MaxMessage)
{
	// no input queue...
	thread_message_queue_tt* queue = current->queue;
	if (!queue)
		return FALSE;

	NTSTATUS r = verify_for_write( Message, sizeof *Message );
	if (r != STATUS_SUCCESS)
		return FALSE;

	MSG msg;
	memset( &msg, 0, sizeof msg );
	if (queue->get_message( msg, Window, MinMessage, MaxMessage ))
		copy_to_user( Message, &msg, sizeof msg );

	if (option_trace)
	{
		fprintf(stderr, "%04lx: %s\n", current->trace_id(), __FUNCTION__);
		fprintf(stderr, " msg.hwnd    = %p\n", msg.hwnd);
		fprintf(stderr, " msg.message = %08x (%s)\n", msg.message, get_message_name(msg.message));
		fprintf(stderr, " msg.wParam  = %08x\n", msg.wParam);
		fprintf(stderr, " msg.lParam  = %08lx\n", msg.lParam);
		fprintf(stderr, " msg.time    = %08lx\n", msg.time);
		fprintf(stderr, " msg.pt.x    = %08lx\n", msg.pt.x);
		fprintf(stderr, " msg.pt.y    = %08lx\n", msg.pt.y);
	}

	return msg.message != WM_QUIT;
}

BOOLEAN NTAPI NtUserPostMessage( HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	thread_t*& thread = win->get_win_thread();
	assert(thread != NULL);

	return thread->queue->post_message( Window, Message, Wparam, Lparam );
}

BOOLEAN NTAPI NtUserPeekMessage( PMSG Message, HWND Window, UINT MaxMessage, UINT MinMessage, UINT Remove)
{
	thread_message_queue_tt* queue = current->queue;
	if (!queue)
		return FALSE;

	NTSTATUS r = verify_for_write( Message, sizeof *Message );
	if (r != STATUS_SUCCESS)
		return FALSE;

	MSG msg;
	memset( &msg, 0, sizeof msg );
	BOOL ret = queue->get_message_no_wait( msg, Window, MinMessage, MaxMessage );
	if (ret)
		copy_to_user( Message, &msg, sizeof msg );

	return ret;
}

UINT NTAPI NtUserSetTimer( HWND Window, UINT Identifier, UINT Elapse, PVOID TimerProc )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	thread_t*& thread = win->get_win_thread();
	assert(thread != NULL);

	return thread->queue->set_timer( Window, Identifier, Elapse, TimerProc );
}

BOOLEAN NTAPI NtUserKillTimer( HWND Window, UINT Identifier )
{
	window_tt *win = window_from_handle( Window );
	if (!win)
		return FALSE;

	thread_t*& thread = win->get_win_thread();
	assert(thread != NULL);

	return thread->queue->kill_timer( Window, Identifier );
}

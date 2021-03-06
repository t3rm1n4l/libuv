/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <signal.h>

#include "uv.h"
#include "internal.h"
#include "handle-inl.h"
#include "req-inl.h"


RB_HEAD(uv_signal_tree_s, uv_signal_s);

static struct uv_signal_tree_s uv__signal_tree = RB_INITIALIZER(uv__signal_tree);
static ssize_t volatile uv__signal_control_handler_refs = 0;
static CRITICAL_SECTION uv__signal_lock;


void uv_signals_init() {
  InitializeCriticalSection(&uv__signal_lock);
}


static int uv__signal_compare(uv_signal_t* w1, uv_signal_t* w2) {
  /* Compare signums first so all watchers with the same signnum end up */
  /* adjacent. */
  if (w1->signum < w2->signum) return -1;
  if (w1->signum > w2->signum) return 1;

  /* Sort by loop pointer, so we can easily look up the first item after */
  /* { .signum = x, .loop = NULL } */
  if ((uintptr_t) w1->loop < (uintptr_t) w2->loop) return -1;
  if ((uintptr_t) w1->loop > (uintptr_t) w2->loop) return 1;

  if ((uintptr_t) w1 < (uintptr_t) w2) return -1;
  if ((uintptr_t) w1 > (uintptr_t) w2) return 1;

  return 0;
}


RB_GENERATE_STATIC(uv_signal_tree_s, uv_signal_s, tree_entry, uv__signal_compare);


/*
 * Dispatches signal {signum} to all active uv_signal_t watchers in all loops.
 * Returns 1 if the signal was dispatched to any watcher, or 0 if there were
 * no active signal watchers observing this signal.
 */
int uv__signal_dispatch(int signum) {
  uv_signal_t lookup;
  uv_signal_t* handle;
  int dispatched = 0;

  EnterCriticalSection(&uv__signal_lock);

  lookup.signum = signum;
  lookup.loop = NULL;

  for (handle = RB_NFIND(uv_signal_tree_s, &uv__signal_tree, &lookup);
       handle != NULL && handle->signum == signum;
       handle = RB_NEXT(uv_signal_tree_s, &uv__signal_tree, handle)) {
    unsigned long previous = InterlockedExchange(&handle->pending_signum, signum);

    if (!previous) {
      POST_COMPLETION_FOR_REQ(handle->loop, &handle->signal_req);
    }

    dispatched = 1;
  }

  LeaveCriticalSection(&uv__signal_lock);

  return dispatched;
}


static BOOL WINAPI uv__signal_control_handler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
      return uv__signal_dispatch(SIGINT);

    case CTRL_BREAK_EVENT:
      return uv__signal_dispatch(SIGBREAK);

    case CTRL_CLOSE_EVENT:
      if (uv__signal_dispatch(SIGHUP)) {
        /* Windows will terminate the process after the control handler */
        /* returns. After that it will just terminate our process. Therefore */
        /* block the signal handler so the main loop has some time to pick */
        /* up the signal and do something for a few seconds. */
        Sleep(INFINITE);
        return TRUE;
      }
      return FALSE;

    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      /* These signals are only sent to services. Services have their own */
      /* notification mechanism, so there's no point in handling these. */

    default:
      /* We don't handle these. */
      return FALSE;
  }
}


static uv_err_t uv__signal_register_control_handler() {
  /* When this function is called, the uv__signal_lock must be held. */

  /* If the console control handler has already been hooked, just add a */
  /* reference. */
  if (uv__signal_control_handler_refs > 0)
    return uv_ok_;

  if (!SetConsoleCtrlHandler(uv__signal_control_handler, TRUE))
    return uv__new_sys_error(GetLastError());

  uv__signal_control_handler_refs++;

  return uv_ok_;
}


static uv_err_t uv__signal_unregister_control_handler() {
  /* When this function is called, the uv__signal_lock must be held. */

  /* Don't unregister if the number of console control handlers exceeds one. */
  /* Just remove a reference in that case. */
  if (uv__signal_control_handler_refs > 1) {
    uv__signal_control_handler_refs--;
    return uv_ok_;
  }

  assert(uv__signal_control_handler_refs == 1);

  if (!SetConsoleCtrlHandler(uv__signal_control_handler, FALSE))
    return uv__new_sys_error(GetLastError());

  uv__signal_control_handler_refs--;

  return uv_ok_;
}


static uv_err_t uv__signal_register(int signum) {
  switch (signum) {
    case SIGINT:
    case SIGBREAK:
    case SIGHUP:
      return uv__signal_register_control_handler();

    default:
      /* Unsupported signal */
      return uv__new_artificial_error(UV_ENOTSUP);
  }
}


static uv_err_t uv__signal_unregister(int signum) {
  switch (signum) {
    case SIGINT:
    case SIGBREAK:
    case SIGHUP:
      return uv__signal_unregister_control_handler();

    default:
      /* Unsupported signal */
      return uv__new_artificial_error(UV_ENOTSUP);
  }
}


int uv_signal_init(uv_loop_t* loop, uv_signal_t* handle) {
  uv_req_t* req;

  uv__handle_init(loop, (uv_handle_t*) handle, UV_SIGNAL);
  handle->pending_signum = 0;
  handle->signum = 0;
  handle->signal_cb = NULL;

  req = &handle->signal_req;
  uv_req_init(loop, req);
  req->type = UV_SIGNAL_REQ;
  req->data = handle;

  uv__handle_start(handle);

  return 0;
}


int uv_signal_stop(uv_signal_t* handle) {
  uv_err_t err;
  uv_signal_t* removed_handle;

  /* If the watcher wasn't started, this is a no-op. */
  if (handle->signum == 0)
    return 0;

  EnterCriticalSection(&uv__signal_lock);

  err = uv__signal_unregister(handle->signum);
  if (err.code != UV_OK) {
    /* Uh-oh, didn't work. */
    handle->loop->last_err = err;
    LeaveCriticalSection(&uv__signal_lock);
    return -1;
  }

  removed_handle = RB_REMOVE(uv_signal_tree_s, &uv__signal_tree, handle);
  assert(removed_handle == handle);

  LeaveCriticalSection(&uv__signal_lock);

  handle->signum = 0;
  uv__handle_stop(handle);

  return 0;
}


int uv_signal_start(uv_signal_t* handle, uv_signal_cb signal_cb, int signum) {
  uv_err_t err;


  /* Short circuit: if the signal watcher is already watching {signum} don't */
  /* go through the process of deregistering and registering the handler. */
  /* Additionally, this avoids pending signals getting lost in the (small) */
  /* time frame that handle->signum == 0. */
  if (signum == handle->signum) {
    handle->signal_cb = signal_cb;
    return 0;
  }

  /* If the signal handler was already active, stop it first. */
  if (handle->signum != 0 &&
      uv_signal_stop(handle) < 0) {
    return -1;
  }

  EnterCriticalSection(&uv__signal_lock);

  err = uv__signal_register(signum);
  if (err.code != UV_OK) {
    /* Uh-oh, didn't work. */
    handle->loop->last_err = err;
    LeaveCriticalSection(&uv__signal_lock);
    return -1;
  }

  handle->signum = signum;
  RB_INSERT(uv_signal_tree_s, &uv__signal_tree, handle);

  LeaveCriticalSection(&uv__signal_lock);

  handle->signal_cb = signal_cb;
  uv__handle_start(handle);

  return 0;
}


void uv_process_signal_req(uv_loop_t* loop, uv_signal_t* handle,
    uv_req_t* req) {
  unsigned long dispatched_signum;

  assert(handle->type == UV_SIGNAL);
  assert(req->type == UV_SIGNAL_REQ);

  dispatched_signum = InterlockedExchange(&handle->pending_signum, 0);
  assert(dispatched_signum != 0);

  /* Check if the pending signal equals the signum that we are watching for. */
  /* These can get out of sync when the handler is stopped and restarted */
  /* while the signal_req is pending. */
  if (dispatched_signum == handle->signum)
    handle->signal_cb(handle, dispatched_signum);

  if (handle->flags & UV_HANDLE_CLOSING) {
    /* When it is closing, it must be stopped at this point. */
    assert(handle->signum == 0);
    uv_want_endgame(loop, (uv_handle_t*) handle);
  }
}


void uv_signal_close(uv_loop_t* loop, uv_signal_t* handle) {
  uv_signal_stop(handle);

  if (handle->pending_signum == 0) {
    uv__handle_start(handle);
    uv_want_endgame(loop, (uv_handle_t*) handle);
  }
}


void uv_signal_endgame(uv_loop_t* loop, uv_signal_t* handle) {
  assert(handle->flags & UV_HANDLE_CLOSING);
  assert(!(handle->flags & UV_HANDLE_CLOSED));

  assert(handle->signum = 0);
  assert(handle->pending_signum = 0);

  handle->flags |= UV_HANDLE_CLOSED;

  uv__handle_stop(handle);
  uv__handle_close(handle);
}

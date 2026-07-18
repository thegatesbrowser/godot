/* SPDX-License-Identifier: MPL-2.0 */

#include "precompiled.hpp"
#if defined ZMQ_IOTHREAD_POLLER_USE_EPOLL
#include "epoll.hpp"

#if !defined ZMQ_HAVE_WINDOWS
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <algorithm>
#include <new>

#include "macros.hpp"
#include "err.hpp"
#include "config.hpp"
#include "i_poll_events.hpp"

#ifdef ZMQ_HAVE_WINDOWS
const zmq::epoll_t::epoll_fd_t zmq::epoll_t::epoll_retired_fd =
  INVALID_HANDLE_VALUE;
#endif

zmq::epoll_t::epoll_t (const zmq::thread_ctx_t &ctx_) :
    worker_poller_base_t (ctx_)
{
#ifdef ZMQ_IOTHREAD_POLLER_USE_EPOLL_CLOEXEC
    //  Setting this option result in sane behaviour when exec() functions
    //  are used. Old sockets are closed and don't block TCP ports, avoid
    //  leaks, etc.
    _epoll_fd = epoll_create1 (EPOLL_CLOEXEC);
#else
    _epoll_fd = epoll_create (1);
#endif
    errno_assert (_epoll_fd != epoll_retired_fd);
}

zmq::epoll_t::~epoll_t ()
{
    //  Wait till the worker thread exits.
    stop_worker ();

#ifdef ZMQ_HAVE_WINDOWS
    epoll_close (_epoll_fd);
#else
    close (_epoll_fd);
#endif
    for (retired_t::iterator it = _retired.begin (), end = _retired.end ();
         it != end; ++it) {
        LIBZMQ_DELETE (*it);
    }
#if defined ZMQ_HAVE_WINDOWS
    //  TheGates patch: Queued native-event entries outlive the worker loop.
    for (retired_t::iterator it = _pending_retired.begin (),
                             end = _pending_retired.end ();
         it != end; ++it) {
        LIBZMQ_DELETE (*it);
    }
#endif
}

zmq::epoll_t::handle_t zmq::epoll_t::add_fd (fd_t fd_, i_poll_events *events_)
{
    check_thread ();
    poll_entry_t *pe = new (std::nothrow) poll_entry_t;
    alloc_assert (pe);

    //  The memset is not actually needed. It's here to prevent debugging
    //  tools to complain about using uninitialised memory.
    memset (pe, 0, sizeof (poll_entry_t));

    pe->fd = fd_;
    pe->ev.events = 0;
    pe->ev.data.ptr = pe;
    pe->events = events_;
#if defined ZMQ_HAVE_WINDOWS
    //  TheGates patch: Initialize fields used only by native event poll sources.
    pe->epoll_fd = _epoll_fd;
    pe->wait_handle = NULL;
    pe->pending_posts = 0;
    pe->native_event = false;
#endif

    const int rc = epoll_ctl (_epoll_fd, EPOLL_CTL_ADD, fd_, &pe->ev);
    errno_assert (rc != -1);

    //  Increase the load metric of the thread.
    adjust_load (1);

    return pe;
}

#if defined ZMQ_HAVE_WINDOWS
//  TheGates patch: Bridge a kernel event into wepoll's IOCP without a socketpair.
zmq::epoll_t::handle_t zmq::epoll_t::add_event (fd_t event_,
                                                 i_poll_events *events_)
{
    check_thread ();
    poll_entry_t *pe = new (std::nothrow) poll_entry_t;
    alloc_assert (pe);
    memset (pe, 0, sizeof (poll_entry_t));

    pe->fd = event_;
    pe->ev.events = 0;
    pe->ev.data.ptr = pe;
    pe->events = events_;
    pe->epoll_fd = _epoll_fd;
    pe->wait_handle = NULL;
    pe->pending_posts = 0;
    pe->native_event = true;

    adjust_load (1);
    return pe;
}

//  TheGates patch: Post native-event readiness into the same IOCP used by wepoll.
VOID CALLBACK zmq::epoll_t::event_callback (PVOID context_, BOOLEAN)
{
    poll_entry_t *pe = static_cast<poll_entry_t *> (context_);
    InterlockedIncrement (&pe->pending_posts);
    const BOOL rc = PostQueuedCompletionStatus (
      pe->epoll_fd, EPOLLIN, reinterpret_cast<ULONG_PTR> (pe), NULL);
    if (!rc)
        InterlockedDecrement (&pe->pending_posts);
    win_assert (rc != 0);
}

//  TheGates patch: Arm a one-shot wait so the manual-reset event remains level-triggered.
void zmq::epoll_t::register_event (poll_entry_t *entry_)
{
    const BOOL rc = RegisterWaitForSingleObject (
      &entry_->wait_handle, reinterpret_cast<HANDLE> (entry_->fd),
      event_callback, entry_, INFINITE, WT_EXECUTEONLYONCE);
    win_assert (rc != 0);
}

//  TheGates patch: Drain the wait callback before retiring its poll entry.
void zmq::epoll_t::unregister_event (poll_entry_t *entry_)
{
    if (entry_->wait_handle == NULL)
        return;
    const BOOL rc =
      UnregisterWaitEx (entry_->wait_handle, INVALID_HANDLE_VALUE);
    win_assert (rc != 0);
    entry_->wait_handle = NULL;
}
#endif

void zmq::epoll_t::rm_fd (handle_t handle_)
{
    check_thread ();
    poll_entry_t *pe = static_cast<poll_entry_t *> (handle_);
#if defined ZMQ_HAVE_WINDOWS
    if (pe->native_event) {
        unregister_event (pe);
        pe->fd = retired_fd;
        if (InterlockedCompareExchange (&pe->pending_posts, 0, 0) == 0)
            _retired.push_back (pe);
        else
            _pending_retired.push_back (pe);
        adjust_load (-1);
        return;
    }
#endif
    const int rc = epoll_ctl (_epoll_fd, EPOLL_CTL_DEL, pe->fd, &pe->ev);
    errno_assert (rc != -1);
    pe->fd = retired_fd;
    _retired.push_back (pe);

    //  Decrease the load metric of the thread.
    adjust_load (-1);
}

void zmq::epoll_t::set_pollin (handle_t handle_)
{
    check_thread ();
    poll_entry_t *pe = static_cast<poll_entry_t *> (handle_);
    pe->ev.events |= EPOLLIN;
#if defined ZMQ_HAVE_WINDOWS
    if (pe->native_event) {
        register_event (pe);
        return;
    }
#endif
    const int rc = epoll_ctl (_epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void zmq::epoll_t::reset_pollin (handle_t handle_)
{
    check_thread ();
    poll_entry_t *pe = static_cast<poll_entry_t *> (handle_);
    pe->ev.events &= ~(static_cast<uint32_t> (EPOLLIN));
#if defined ZMQ_HAVE_WINDOWS
    if (pe->native_event) {
        unregister_event (pe);
        return;
    }
#endif
    const int rc = epoll_ctl (_epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void zmq::epoll_t::set_pollout (handle_t handle_)
{
    check_thread ();
    poll_entry_t *pe = static_cast<poll_entry_t *> (handle_);
#if defined ZMQ_HAVE_WINDOWS
    //  TheGates patch: Native signaler events are read-only poll sources.
    zmq_assert (!pe->native_event);
#endif
    pe->ev.events |= EPOLLOUT;
    const int rc = epoll_ctl (_epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void zmq::epoll_t::reset_pollout (handle_t handle_)
{
    check_thread ();
    poll_entry_t *pe = static_cast<poll_entry_t *> (handle_);
#if defined ZMQ_HAVE_WINDOWS
    //  TheGates patch: Native signaler events are read-only poll sources.
    zmq_assert (!pe->native_event);
#endif
    pe->ev.events &= ~(static_cast<uint32_t> (EPOLLOUT));
    const int rc = epoll_ctl (_epoll_fd, EPOLL_CTL_MOD, pe->fd, &pe->ev);
    errno_assert (rc != -1);
}

void zmq::epoll_t::stop ()
{
    check_thread ();
}

int zmq::epoll_t::max_fds ()
{
    return -1;
}

void zmq::epoll_t::loop ()
{
    epoll_event ev_buf[max_io_events];

    while (true) {
        //  Execute any due timers.
        const int timeout = static_cast<int> (execute_timers ());

        if (get_load () == 0) {
            if (timeout == 0)
                break;

            // TODO sleep for timeout
            continue;
        }

        //  Wait for events.
        const int n = epoll_wait (_epoll_fd, &ev_buf[0], max_io_events,
                                  timeout ? timeout : -1);
        if (n == -1) {
            errno_assert (errno == EINTR);
            continue;
        }

        for (int i = 0; i < n; i++) {
            poll_entry_t *const pe =
              static_cast<poll_entry_t *> (ev_buf[i].data.ptr);

            if (NULL == pe)
                continue;
#if defined ZMQ_HAVE_WINDOWS
            //  TheGates patch: Native events may be retired while their IOCP post is queued.
            if (pe->native_event) {
                if (pe->fd != retired_fd && pe->events != NULL)
                    pe->events->in_event ();
                if (pe->fd != retired_fd) {
                    unregister_event (pe);
                    register_event (pe);
                }
                InterlockedDecrement (&pe->pending_posts);
                continue;
            }
#endif
            if (NULL == pe->events)
                continue;
            if (pe->fd == retired_fd)
                continue;
            if (ev_buf[i].events & (EPOLLERR | EPOLLHUP))
                pe->events->in_event ();
            if (pe->fd == retired_fd)
                continue;
            if (ev_buf[i].events & EPOLLOUT)
                pe->events->out_event ();
            if (pe->fd == retired_fd)
                continue;
            if (ev_buf[i].events & EPOLLIN)
                pe->events->in_event ();
        }

#if defined ZMQ_HAVE_WINDOWS
        //  TheGates patch: Release removed events after their queued IOCP post is consumed.
        for (retired_t::iterator it = _pending_retired.begin ();
             it != _pending_retired.end ();) {
            poll_entry_t *pe = *it;
            if (InterlockedCompareExchange (&pe->pending_posts, 0, 0) == 0) {
                _retired.push_back (pe);
                it = _pending_retired.erase (it);
            } else
                ++it;
        }
#endif

        //  Destroy retired event sources.
        for (retired_t::iterator it = _retired.begin (), end = _retired.end ();
             it != end; ++it) {
            LIBZMQ_DELETE (*it);
        }
        _retired.clear ();
    }
}

#endif

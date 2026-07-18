/* SPDX-License-Identifier: MPL-2.0 */

//  TheGates patch: Verify the Windows signaler works without Winsock providers.

#include "zmq.h"
#include "signaler.hpp"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#endif

static void set_timeout (void *socket_)
{
    const int timeout = 5000;
    int rc = zmq_setsockopt (socket_, ZMQ_RCVTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);
    rc = zmq_setsockopt (socket_, ZMQ_SNDTIMEO, &timeout, sizeof timeout);
    assert (rc == 0);
}

static void round_trip (void *server_, void *client_)
{
    static const char payload[] = "signaler-test";
    int rc = zmq_send (client_, payload, sizeof payload, 0);
    assert (rc == sizeof payload);

    char received[sizeof payload];
    rc = zmq_recv (server_, received, sizeof received, 0);
    assert (rc == sizeof received);
    assert (memcmp (received, payload, sizeof payload) == 0);
}

static void test_signaler_without_winsock ()
{
#if defined ZMQ_HAVE_WINDOWS
    zmq::signaler_t signaler;
    assert (signaler.valid ());

    const HANDLE event = reinterpret_cast<HANDLE> (signaler.get_fd ());
    assert (WaitForSingleObject (event, 0) == WAIT_TIMEOUT);

    signaler.send ();
    assert (signaler.wait (1000) == 0);
    assert (WaitForSingleObject (event, 0) == WAIT_OBJECT_0);

    signaler.recv ();
    assert (WaitForSingleObject (event, 0) == WAIT_TIMEOUT);

    errno = 0;
    assert (signaler.recv_failable () == -1);
    assert (errno == EAGAIN);
#endif
}

static void test_pair_round_trip (const char *endpoint_)
{
    void *context = zmq_ctx_new ();
    assert (context != NULL);

    void *server = zmq_socket (context, ZMQ_PAIR);
    void *client = zmq_socket (context, ZMQ_PAIR);
    assert (server != NULL);
    assert (client != NULL);
    set_timeout (server);
    set_timeout (client);

    int rc = zmq_bind (server, endpoint_);
    assert (rc == 0);
    rc = zmq_connect (client, endpoint_);
    assert (rc == 0);

    round_trip (server, client);

    rc = zmq_close (client);
    assert (rc == 0);
    rc = zmq_close (server);
    assert (rc == 0);
    rc = zmq_ctx_term (context);
    assert (rc == 0);
}

int main ()
{
    test_signaler_without_winsock ();
    test_pair_round_trip ("inproc://signaler-windows-event");

#if defined ZMQ_HAVE_WINDOWS
    char ipc_endpoint[128];
    const int count = snprintf (
      ipc_endpoint, sizeof ipc_endpoint, "ipc://signaler-windows-event-%lu",
      static_cast<unsigned long> (GetCurrentProcessId ()));
    assert (count > 0 && static_cast<size_t> (count) < sizeof ipc_endpoint);
    test_pair_round_trip (ipc_endpoint);
#endif

    puts ("PASS: signaler send/wait/recv/get_fd");
    puts ("PASS: ZMQ_PAIR inproc round-trip");
#if defined ZMQ_HAVE_WINDOWS
    puts ("PASS: ZMQ_PAIR ipc round-trip");
#endif
    return 0;
}

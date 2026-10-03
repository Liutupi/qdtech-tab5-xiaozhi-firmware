# Local ESP ML307 component override

This is a source-owned copy of `78/esp-ml307` version 3.7.4, upstream commit
`e94d017a6cb6b883af084a212093618c4c94c70f`, under its Apache-2.0 license.
It keeps the original component layout so ESP-IDF can use it through a manifest
`override_path` without editing generated `managed_components/` output.

The local change is confined to `include/tcp.h`, `include/web_socket.h`,
`src/esp/esp_tcp.{h,cc}`, `src/esp/esp_ssl.{h,cc}`, `src/web_socket.cc`, and
`src/http_client.cc`:

- Make the TCP connection flag atomic because the receive and owner tasks both use it.
- Keep the socket descriptor until the receive task has finished its last access
  to the socket object. A passive peer close
  clears the connection flag before its callback returns, so the old destructor's
  flag-only check skipped the join and freed the task's event group too early.
- The task now makes a release-store to `receive_task_finished_` as its final
  access to the object. The owner waits for an acquire-load of that flag before
  freeing resources; no event-group API runs after notifying the owner.
- Use `shutdown()` to wake `recv()`, wait for that handoff, then `close()` under
  the send lock. A ten-second wait only logs; it never releases live state.
- Join an old passive receive task before reconnecting and check task creation.
- Cache the TLS socket descriptor for `shutdown()` without touching a concurrent
  writer's TLS object. The writer and TLS destruction share a send mutex.
  Check TLS receive-task creation as well.
- Join the TCP/TLS receive task before destroying the WebSocket handshake event
  group. Register callbacks before starting the receive task, and wake a pending
  handshake as failed when the transport disconnects.
- In `HttpClient`, join the transport even when its connection flag is already
  false, before deleting the HTTP event group used by receive callbacks. Wake
  readers and header waiters promptly on disconnect, avoid lost wakeups under
  body backpressure, preserve endpoint checks for keep-alive reuse, and finish
  explicit zero-length responses without waiting for a connection close.
- Add `WebSocket::Abort()` so the protocol worker can shut down a blocked send
  before waiting for sender tasks to drain.

The owning application must serialize physical `Disconnect()` calls and release
`EspTcp` or `EspSsl` from a different task after an
`OnStream` or `OnDisconnected` callback returns. Destroying the socket from inside
its own receive callback is unsupported because that callback executes on the
socket object's stack. The XiaoZhi protocol layer schedules physical close on its
background worker.

When updating the upstream component, compare the changed files against
the new release and remove this override if upstream has a safe task join.

# Event-driven operation completion

`execution.wait` retains its nonblocking default. An explicit `timeout_ms`
from 1 through 60000 now waits for operation completion or that host timeout.
After the timeout it returns the operation's current running/completed state;
it never pauses or resumes the guest. Unknown/expired operation IDs and invalid
parameter types/ranges remain errors.

The guest-owning thread validates the request and publishes its response. Only
the socket thread waits on a host condition variable. Completion increments a
generation under the waiter mutex, preventing a lost notification between the
initial running query and entry into the wait. After wake/timeout, a second
owner-thread query obtains the result. No guest or operation JSON state is read
by a socket thread, and other connections can still pause or inspect execution.
Already completed operations return immediately. Notifications emit no response
and do not request a blocking wait. Guest clocks and instruction coverage are
unchanged. The existing 64-entry operation history still bounds retention.

This follows the event-driven observation principle used by MZ800emu's debugger
event polling, while preserving this server's owner-thread architecture.

Run `python tests/rpc_wait.py --executable <fuse.exe> --output <fresh-directory>`
for real native API controls: default polling, bounded running timeout, concurrent
waiters plus separate-controller pause, pure completed reads, 100 fast completion
races and invalid inputs. The generated 48K snapshot executes only `JR $`.
These controls are not a CPU model, application replay or hardware proof.

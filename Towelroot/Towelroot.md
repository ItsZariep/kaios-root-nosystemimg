This is the Towelroot exploit (CVE-2014-3153), and the log shows it failed to trigger on your KaiOS device. The kernel is 3.4.67 (compiled November 2024), which is almost certainly patched against this 2014 vulnerability.

- The exploit hung after "Using hacked_node_alt": This means the kernel waiter list corruption didn't work. The code reached the point where it created fake waiter nodes in userspace, but when it tried to verify the list was corrupted (by checking if hacked_node changed), the values didn't match.


Output:

```
130|shell@GQWERTY:/data/local/tmp $ ./busybox taskset 0x1 ./futex
uid 2000

[CONTROLLER] Controller started with PID 2737

[TOWEL] Creating socket

[TOWEL] Starting exploitation

[TRIGGER] Trigger pid ab5

[STACK MODIFIER] Modifier started with tid 2740

[ACCEPT SOCKET] Client accepted!

[STACK MODIFIER] Going in WAIT_REQUEUE

[TRIGGER] Stack modifier requeued

[WAKE_ACTIONTHREAD] Starting actionthread

[MAKE ACTION] Adding lock with prio 3 and tid 2742

[WAKE_ACTIONTHREAD] Continuing actionthread

[WAKE_ACTIONTHREAD] Starting actionthread

[MAKE ACTION] Adding lock with prio 6 and tid 2743

[WAKE_ACTIONTHREAD] Continuing actionthread

[WAKE_ACTIONTHREAD] Starting actionthread

[MAKE ACTION] Adding lock with prio 7 and tid 2744

[WAKE_ACTIONTHREAD] Continuing actionthread

[WAKE_ACTIONTHREAD] Starting actionthread

[MAKE ACTION] Adding lock with prio 11 and tid 2745

[MAKE ACTION] Lock with prio 11 and tid 2745 returned

[MAKE ACTION] Killing prio 11

[KILLER] Thread with pid 2738 and tid 2738 is going to exit

[WAKE_ACTIONTHREAD] Continuing actionthread

Alarm clock
[MAKE ACTION] Killing prio 7

[KILLER] Thread with pid 2738 and tid 2744 is going to exit

[MAKE ACTION] All done!

142|shell@GQWERTY:/data/local/tmp $ [TRIGGER] Using hacked_node_alt.

[WAKE_ACTIONTHREAD] Starting actionthread


142|shell@GQWERTY:/data/local/tmp $ ./busybox uname -a
Linux localhost 3.4.67 #1 SMP PREEMPT Tue Nov 26 18:03:57 CST 2024 armv7l GNU/Linux
shell@GQWERTY:/data/local/tmp $ 
```
# Point actions

A point action is a short script attached to a point that runs when a Modbus client reads or writes
it: a counter that grows with every poll, a register derived from another one at read time, a coil
that resets a counter when written. It is the configuration-side equivalent of the custom actions of
other simulators, written in the same statement and expression language as
[machines](../machines/index.md), compiled when the config is loaded and run without allocating.
Type in the box to narrow the list.

An action is written inline in an [on-read](on-read.md) or [on-write](on-write.md) point option, or
defined once with [define-action](define-action.md), given [params](param.md) and reused on many
points. Inside it, [self](self.md) is the attached point.

- Actions run only for Modbus client requests. Host reads and writes (the C and Python libraries, the
  HTTP control API, the web UI, `--show-registers`) never run them.
- Every use is its own instance with its own params and variables.
- A failed action (budget exhausted, division by zero, a point that cannot be read or written) is
  logged once per instance and failure kind and the request is answered with exception 04.
- Writing a point from an action works like a host write: the point freezes like after a client
  write, and no hook, machine on-write handler or other action runs because of it.
- A device holds at most 256 action instances and a file at most 64 definitions. The statement,
  expression and variable limits of a [machine](../machines/statements.md) apply to each action.

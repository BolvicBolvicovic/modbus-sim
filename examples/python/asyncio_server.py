#!/usr/bin/env python3
"""Serves a device inside an asyncio event loop and talks to it with AsyncClient.

No background thread: Server.serve() steps the server on the loop, woken by the readiness of the
server's sockets, next to the other tasks of the program (here a ticker that moves a value and a
client that reads it). Exit status 0 means every check passed.

From a checkout:  make python && PYTHONPATH=python python3 examples/python/asyncio_server.py
"""
import asyncio
import socket
import sys

import mogbus


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def main() -> int:
    failures = 0

    def check(what, got, want):
        nonlocal failures
        ok = got == want
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ("" if ok else f" (wanted {want!r})"))

    with mogbus.Context() as ctx:
        dev = ctx.add_device(1, "async-station")
        dev.add_point("holding", 0, "u16", 100)
        dev.add_point("holding", 1, "u16", 0)
        reads = []
        dev.set_access_hook(lambda access: reads.append(access.address))   # runs on the loop's thread

        port = free_port()
        async with ctx.serve(port=port).running():
            async def ticker():
                for n in range(1, 4):
                    dev.set_value("holding", 0, 100 + n)                   # the host side, in the loop
                    await asyncio.sleep(0.05)

            async with mogbus.AsyncClient(port=port) as plc:
                await asyncio.gather(ticker(), plc.write_register(1, 7))
                check("write seen by the host", dev.get_value("holding", 1), 7)
                check("value the ticker left", await plc.read_holding_registers(0, 2), [103, 7])
                try:
                    await plc.read_holding_registers(5000, 1)
                except mogbus.ModbusException as exc:
                    check("undefined address", exc.code, 2)
        check("hook saw the write and the read", reads, [1, 0])

        # Two servers of one context, driven by one coroutine.
        tcp, udp = free_port(), free_port()
        ctx.serve(port=tcp)
        ctx.serve(mode="udp", port=udp)
        task = asyncio.create_task(ctx.serve_all())
        async with mogbus.AsyncClient(port=tcp) as a, mogbus.AsyncClient(mode="udp", port=udp) as b:
            check("tcp and udp", await asyncio.gather(a.read_holding_registers(1, 1),
                                                      b.read_holding_registers(1, 1)), [[7], [7]])
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
    return failures


if __name__ == "__main__":
    sys.exit(1 if asyncio.run(main()) else 0)

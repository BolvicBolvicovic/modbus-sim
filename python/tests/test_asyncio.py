"""asyncio integration: Server.serve()/running(), Context.serve_all() and AsyncClient."""
import asyncio
import inspect
import io
import socket
import threading
import time
import unittest
from contextlib import redirect_stderr

import mogbus
from mogbus import AsyncClient, Client, ModbusException, MogbusError


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def port_is_free(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind(("127.0.0.1", port))
        except OSError:
            return False
        return True


class AsyncTestCase(unittest.IsolatedAsyncioTestCase):
    def make_ctx(self, value=1234):
        ctx = mogbus.Context()
        self.addCleanup(ctx.close)
        dev = ctx.add_device(1, "async-test")
        dev.add_point("holding", 0, "u16", value)
        dev.add_point("holding", 1, "u16", 0)
        dev.add_point("coil", 0, "bool", 1)
        return ctx, dev


class ServeTest(AsyncTestCase):
    async def test_serve_answers_an_async_client(self):
        ctx, _ = self.make_ctx()
        port = free_port()
        async with ctx.serve(port=port).running():
            async with AsyncClient(port=port) as c:
                self.assertEqual(await c.read_holding_registers(0, 1), [1234])
                await c.write_register(1, 77)
                self.assertEqual(await c.read_holding_registers(0, 2), [1234, 77])
                await c.write_coil(0, False)
                self.assertEqual(await c.read_coils(0, 1), [False])
                with self.assertRaises(ModbusException) as raised:
                    await c.read_holding_registers(500, 1)
                self.assertEqual(raised.exception.code, 2)
        self.assertTrue(port_is_free(port))

    async def test_calls_are_serialized_and_gathered(self):
        ctx, _ = self.make_ctx()
        port = free_port()
        async with ctx.serve(port=port).running():
            async with AsyncClient(port=port) as c:
                results = await asyncio.gather(*[c.read_holding_registers(0, 1) for _ in range(20)])
        self.assertEqual(results, [[1234]] * 20)

    async def test_loop_stays_responsive_while_serving(self):
        ctx, _ = self.make_ctx()
        ticks = 0
        async with ctx.serve(port=free_port()).running():
            end = time.monotonic() + 0.3
            while time.monotonic() < end:
                await asyncio.sleep(0.01)
                ticks += 1
        self.assertGreater(ticks, 10)

    async def test_two_servers_in_one_loop(self):
        ctx, _ = self.make_ctx()
        tcp_port = free_port()
        udp_port = free_port(socket.SOCK_DGRAM)
        ctx.serve(port=tcp_port)
        ctx.serve(mode="udp", port=udp_port)
        task = asyncio.ensure_future(ctx.serve_all())
        try:
            async with AsyncClient(port=tcp_port) as a, AsyncClient(mode="udp", port=udp_port) as b:
                got = await asyncio.gather(a.read_holding_registers(0, 1), b.read_holding_registers(0, 1))
            self.assertEqual(got, [[1234], [1234]])
        finally:
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
        ctx.close()
        self.assertTrue(port_is_free(tcp_port))

    async def test_two_contexts_in_one_loop(self):
        first, _ = self.make_ctx(1)
        second, _ = self.make_ctx(2)
        p1, p2 = free_port(), free_port()
        async with first.serve(port=p1).running(), second.serve(port=p2).running():
            async with AsyncClient(port=p1) as a, AsyncClient(port=p2) as b:
                self.assertEqual(await a.read_holding_registers(0, 1), [1])
                self.assertEqual(await b.read_holding_registers(0, 1), [2])

    async def test_serve_all_ends_when_every_server_stopped(self):
        ctx, _ = self.make_ctx()
        s1 = ctx.serve(port=free_port())
        s2 = ctx.serve(port=free_port())
        task = asyncio.ensure_future(ctx.serve_all())
        await asyncio.sleep(0.05)
        s1.stop()
        s2.stop()
        self.assertEqual(await asyncio.wait_for(task, 2), 0)

    async def test_serve_all_without_servers_returns(self):
        ctx, _ = self.make_ctx()
        self.assertEqual(await asyncio.wait_for(ctx.serve_all(), 2), 0)

    async def test_hook_runs_in_the_loop_thread(self):
        ctx, dev = self.make_ctx()
        threads = []
        dev.set_access_hook(lambda access: threads.append(threading.get_ident()))
        port = free_port()
        async with ctx.serve(port=port).running():
            async with AsyncClient(port=port) as c:
                await c.read_holding_registers(0, 1)
        self.assertEqual(threads, [threading.get_ident()])

    async def test_hook_can_refresh_the_value_it_serves(self):
        ctx, dev = self.make_ctx()
        dev.set_access_hook(lambda access: dev.set_value("holding", 0, 4321))
        port = free_port()
        async with ctx.serve(port=port).running():
            async with AsyncClient(port=port) as c:
                self.assertEqual(await c.read_holding_registers(0, 1), [4321])

    async def test_hook_exception_is_answered_and_serving_goes_on(self):
        ctx, dev = self.make_ctx()
        calls = []

        def hook(access):
            calls.append(access)
            if len(calls) == 1:
                raise RuntimeError("boom")

        dev.set_access_hook(hook)
        port = free_port()
        errors = io.StringIO()
        async with ctx.serve(port=port).running():
            async with AsyncClient(port=port) as c:
                with redirect_stderr(errors):
                    with self.assertRaises(ModbusException) as raised:
                        await c.read_holding_registers(0, 1)
                self.assertEqual(raised.exception.code, 4)
                self.assertEqual(await c.read_holding_registers(0, 1), [1234])
        self.assertIn("boom", errors.getvalue())

    async def test_cancellation_stops_the_server_and_frees_the_port(self):
        ctx, _ = self.make_ctx()
        port = free_port()
        server = ctx.serve(port=port)
        task = asyncio.ensure_future(server.serve())
        async with AsyncClient(port=port) as c:
            self.assertEqual(await c.read_holding_registers(0, 1), [1234])
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            with self.assertRaises(MogbusError):
                await c.read_holding_registers(0, 1)
        self.assertEqual(server.step(0), 1)  # the stop was seen
        server.close()
        self.assertTrue(port_is_free(port))

    async def test_stop_ends_serve_at_once(self):
        ctx, _ = self.make_ctx()
        server = ctx.serve(port=free_port())
        task = asyncio.ensure_future(server.serve())
        await asyncio.sleep(0.05)
        started = time.monotonic()
        server.stop()
        self.assertEqual(await asyncio.wait_for(task, 3), 0)
        self.assertLess(time.monotonic() - started, 0.5)

    async def test_stop_from_another_thread(self):
        ctx, _ = self.make_ctx()
        server = ctx.serve(port=free_port())
        task = asyncio.ensure_future(server.serve())
        await asyncio.sleep(0.05)
        threading.Thread(target=server.stop).start()
        self.assertEqual(await asyncio.wait_for(task, 3), 0)

    async def test_hook_may_stop_the_server(self):
        ctx, dev = self.make_ctx()
        port = free_port()
        server = ctx.serve(port=port)
        dev.set_written_hook(lambda access: server.stop())
        task = asyncio.ensure_future(server.serve())
        async with AsyncClient(port=port, timeout_ms=200) as c:
            with self.assertRaises(MogbusError):  # the stop wins over the reply
                await c.write_register(1, 5)
        self.assertEqual(await asyncio.wait_for(task, 3), 0)

    async def test_a_server_cannot_run_twice(self):
        ctx, _ = self.make_ctx()
        server = ctx.serve(port=free_port())
        task = asyncio.ensure_future(server.serve())
        await asyncio.sleep(0.02)
        with self.assertRaises(MogbusError):
            await server.serve()
        with self.assertRaises(MogbusError):
            server.start()
        with self.assertRaises(MogbusError):
            await ctx.serve_all()
        server.stop()
        await task
        threaded = ctx.serve(port=free_port()).start()
        with self.assertRaises(MogbusError):
            await threaded.serve()
        threaded.stop()

    async def test_closed_server_raises(self):
        ctx, _ = self.make_ctx()
        server = ctx.serve(port=free_port())
        server.close()
        with self.assertRaises(MogbusError):
            await server.serve()

    async def test_http_endpoint_is_served_in_the_loop(self):
        ctx, _ = self.make_ctx()
        http = free_port()
        async with ctx.serve(port=free_port(), http_bind_address="127.0.0.1", http_port=http).running():
            reader, writer = await asyncio.open_connection("127.0.0.1", http)
            writer.write(b"GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
            await writer.drain()
            body = await asyncio.wait_for(reader.read(), 2)
            writer.close()
        self.assertTrue(body.startswith(b"HTTP/1.1 200"), body)

    async def test_fallback_without_add_reader(self):
        ctx, _ = self.make_ctx()
        port = free_port()
        loop = asyncio.get_running_loop()

        def unsupported(*args):
            raise NotImplementedError

        loop.add_reader = unsupported
        loop.add_writer = unsupported
        async with ctx.serve(port=port).running(poll_interval=0.002):
            async with AsyncClient(port=port) as c:
                self.assertEqual(await c.read_holding_registers(0, 1), [1234])


class RemainingCallsTest(AsyncTestCase):
    async def test_remaining_calls(self):
        ctx = mogbus.Context()
        self.addCleanup(ctx.close)
        dev = ctx.add_device(1)
        dev.add_point("holding", 0, "u16", 5)
        dev.add_fifo(100, [1, 2, 3])
        dev.add_file_record(1, 0, [10, 11])
        port = free_port()
        async with ctx.serve(port=port).running():
            async with AsyncClient(port=port, byte_timeout_ms=100) as c:
                self.assertEqual(await c.read_exception_status(), 0)
                self.assertEqual(await c.read_fifo(100), [1, 2, 3])
                self.assertEqual(await c.read_file_records([(1, 0, 2)]), [[10, 11]])
                self.assertEqual((await c.read_device_identification(4, 0)).objects, {0: b"mogbus"})
                self.assertEqual(await c.diagnostics(0, b"\x12\x34"), b"\x12\x34")


class ThreadedServerTest(AsyncTestCase):
    async def test_async_client_talks_to_a_threaded_server(self):
        ctx, _ = self.make_ctx()
        port = free_port()
        server = ctx.serve(port=port).start()
        self.addCleanup(server.close)
        async with AsyncClient(port=port) as c:
            self.assertEqual(await c.read_holding_registers(0, 1), [1234])
            await c.write_registers(0, [5, 6])
            self.assertEqual(await c.read_holding_registers(0, 2), [5, 6])

    async def test_async_client_errors(self):
        c = AsyncClient(port=free_port(), timeout_ms=200)
        with self.assertRaises(MogbusError) as raised:
            await c.read_holding_registers(0, 1)
        self.assertEqual(raised.exception.status, mogbus.Status.IO)
        await c.close()
        await c.close()
        with self.assertRaises(MogbusError):
            await c.read_holding_registers(0, 1)

    async def test_unit_and_request(self):
        ctx, _ = self.make_ctx()
        port = free_port()
        server = ctx.serve(port=port).start()
        self.addCleanup(server.close)
        async with AsyncClient(port=port, unit=9) as c:
            with self.assertRaises(ModbusException) as raised:
                await c.read_holding_registers(0, 1)  # no device with unit 9
            self.assertEqual(raised.exception.code, 11)
            self.assertEqual(await c.read_holding_registers(0, 1, unit=1), [1234])
            self.assertEqual(await c.request(3, b"\x00\x00\x00\x01", unit=1), b"\x02\x04\xd2")
            self.assertIn(0, await c.read_device_id(unit=1))


class MirrorTest(unittest.TestCase):
    def test_async_client_has_every_method_of_client(self):
        def public(cls):
            return {n for n, v in inspect.getmembers(cls) if not n.startswith("_") and callable(v)}

        self.assertEqual(public(Client), public(AsyncClient))
        for name in public(Client):
            sync = getattr(Client, name)
            twin = getattr(AsyncClient, name)
            self.assertTrue(inspect.iscoroutinefunction(twin), name)
            self.assertEqual(list(inspect.signature(sync).parameters),
                             list(inspect.signature(twin).parameters), name)
        self.assertEqual(list(inspect.signature(Client.__init__).parameters),
                         list(inspect.signature(AsyncClient.__init__).parameters))


if __name__ == "__main__":
    unittest.main()

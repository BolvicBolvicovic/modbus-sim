# Serial devices on Windows

Serial modes (`rtu-device`, `ascii-device`) run in WSL2 with a USB serial adapter attached to it, and tools that only speak COM ports can reach mogbus through a serial-to-TCP bridge.

Docker Desktop cannot pass Windows COM ports (or USB devices) into containers, so serial modes need
WSL2.

## Attach a USB adapter to WSL2

WSL2 has no access to Windows COM ports, but it can use a USB serial adapter that you attach to it
with [usbipd-win](https://github.com/dorssel/usbipd-win). WSL needs a recent kernel (run
`wsl --update` if needed).

In PowerShell as administrator:

```powershell
winget install --interactive --exact dorssel.usbipd-win
usbipd list
usbipd bind --busid 4-4
usbipd attach --wsl --busid 4-4
```

Replace `4-4` with the bus ID of your adapter from `usbipd list`. Keep a WSL window open while you
attach. As long as the adapter is attached to WSL, Windows cannot use it; `usbipd detach --busid 4-4`
gives it back. The attach has to be repeated after the adapter is unplugged or WSL restarts.

In WSL the device appears as `/dev/ttyUSB0` (FTDI, CH340, CP210x) or `/dev/ttyACM0` (CDC). Your user
needs access to it:

```bash
ls -l /dev/ttyUSB0
sudo usermod -aG dialout $USER     # then open a new WSL shell
```

If the device does not appear, `lsusb` shows whether the adapter arrived; the WSL kernel may lack the
driver for it. Then point mogbus at it:

```scheme
(server
  (mode rtu-device)
  (path "/dev/ttyUSB0")
  (baud 19200)
  (parity even))

(device
  (name "Serial example")
  (unit-id 1)
  (holding 0 (static 42)))
```

Use `(mode ascii-device)` for Modbus ASCII. The serial options are described in
[Modbus RTU](../transports/transport-rtu.md) and the [config reference](../config/format.md).

## Windows software that only talks to a COM port

Some Windows tools can only open a COM port. When mogbus runs in WSL, bridge that COM port to a
network connection:

1. Run mogbus in `rtu-tcp` (or `ascii-tcp`) mode with `(bind "0.0.0.0")`, reachable from Windows as
   described in [Reaching mogbus from Windows](windows-networking.md).
2. On Windows, create a virtual COM port with a third-party driver and connect it to the mogbus TCP
   port with a serial-to-TCP bridge. The open-source com0com (virtual COM port pairs) and hub4com
   (routes data between COM and TCP ports) are meant for this kind of setup, and commercial virtual
   serial port tools do the same. mogbus does not ship or test them; their setup is described in their
   own documentation.
3. Point the Windows tool at the virtual COM port.

The bridge carries the raw RTU or ASCII frames, which is what `rtu-tcp` and `ascii-tcp` speak, so
mogbus needs no serial hardware. The baud rate set in the tool has no effect on the TCP link, and
timing-sensitive RTU clients may see frames split differently than on a real wire.

## See also

- [Modbus RTU over TCP](../transports/transport-rtu-tcp.md)
- [Modbus ASCII over TCP](../transports/transport-ascii-tcp.md)
- [WSL2](windows-wsl2.md)

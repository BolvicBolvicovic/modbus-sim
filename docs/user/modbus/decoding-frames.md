# Decoding frames

Turn a captured Modbus frame (hex from a packet capture, a serial log or a frame hook) into a readable breakdown of its ADU and PDU fields.

`mogbus --decode` reads no config and starts no server. It handles every function code mogbus answers,
in the three ADU formats (MBAP for Modbus TCP and UDP, RTU, ASCII), request or response. It is the
mogbus counterpart of the message decoder that pymodbus ships. The same decoder is a C function,
@ref mogbus_decode_frame "mogbus_decode_frame()", and a Python one, `mogbus.decode_frame()`.

## Giving it frames

```bash
./mogbus --decode "00 01 00 00 00 06 01 03 00 00 00 02"
./mogbus --decode 010300000001840A ':010300000001FB'
./mogbus --decode --framing rtu --direction response "01 03 02 00 0A 38 43"
tshark -r capture.pcap -Y modbus -T fields -e tcp.payload | ./mogbus --decode
```

- **Hex.** With or without spaces; `:`, `,`, `-` and `0x` between the bytes are ignored, so `01:03:00`
  and `0x01,0x03` work. One argument is one frame: quote a frame that has spaces.
- **ASCII text.** An argument that starts with `:` is the ASCII frame itself (its characters), with or
  without the closing CR LF. The same frame as hex bytes (`3A 30 31 ...`) works too.
- **Stdin.** With no argument it reads one frame per line; blank lines and lines starting with `#` are skipped.
- `--log-format json` prints one JSON object per frame (one line each) instead of text.
- The exit status is 0 when every frame decoded cleanly, 1 when one is malformed and 2 for input that is
  not hex. See [Command line](../command-line.md).

## What it prints

A request on Modbus TCP:

```text
bytes: 12
framing: mbap
framing_reason: protocol id is 0 and the length field matches
adu:
  transaction: 1 (0x0001)
  protocol: 0 (0x0000)
  length: 6
  unit: 1
pdu:
  function: 3 (0x03) Read Holding Registers
  direction: request
  direction_reason: length and fields fit the request layout, not the response layout
  address: 0 (0x0000)
  quantity: 2
ok: true
```

A response on RTU, with the registers in decimal and in hex:

```text
bytes: 9
framing: rtu
framing_reason: the CRC matches
adu:
  address: 1 (0x01)
  crc: 63131 (0xF69B)
  crc_ok: true
pdu:
  function: 3 (0x03) Read Holding Registers
  direction: response
  direction_reason: length and byte count fit the response layout, not the request layout
  byte_count: 4
  values: 10 11  [hex 000A 000B]
ok: true
```

An exception answer on ASCII (`:0183027A`):

```text
bytes: 11
framing: ascii
framing_reason: starts with ':' and the LRC matches
adu:
  address: 1 (0x01)
  lrc: 122 (0x7A)
  lrc_ok: true
pdu:
  function: 3 (0x03) Read Holding Registers
  direction: response
  direction_reason: the exception bit of the function code is set
  exception: 2 (0x02) Illegal Data Address
ok: true
```

With `--log-format json` the RTU response is one line (shown wrapped; a name that goes with a number
is the same key with `_name`, as `function` and `function_name`):

```json
{"bytes":9,"framing":"rtu","framing_reason":"the CRC matches",
 "adu":{"address":1,"crc":63131,"crc_ok":true},
 "pdu":{"function":3,"function_name":"Read Holding Registers","direction":"response",
        "direction_reason":"length and byte count fit the response layout, not the request layout",
        "byte_count":4,"values":[10,11]},
 "ok":true}
```

### The fields

| Part | Fields |
|---|---|
| frame | `bytes` (length), `framing`, `framing_reason` (only when the framing was detected), `ok` (false when `errors` is not empty) |
| `adu` MBAP | `transaction`, `protocol`, `length`, `unit` |
| `adu` RTU | `address` (0 is `broadcast`), `crc` (the 16-bit value; the frame carries its low byte first), `crc_ok`, `crc_expected` when it does not match |
| `adu` ASCII | `address`, `lrc`, `lrc_ok`, `lrc_expected` when it does not match |
| `pdu` | `function` and its name, `direction` (`request`, `response` or `unknown`), `direction_reason` (only when the direction was inferred), then the fields of the function code |

The PDU fields of each function code are named after the [function code pages](function-codes.md):

| Code | Request fields | Response fields |
|---|---|---|
| 01, 02 | `address`, `quantity` | `byte_count`, `data`, `bits` (bit 0 of the first byte first) |
| 03, 04 | `address`, `quantity` | `byte_count`, `values` |
| 05, 06 | `address`, `value` (a coil as `ON` or `OFF`) | the same, an echo |
| 07 | none | `output_data` |
| 08 | `sub_function` and `sub_function_text`, `data` | the same |
| 0B | none | `status` (`busy`, `ready`), `event_count` |
| 0C | none | `byte_count`, `status`, `event_count`, `message_count`, `events` (each named) |
| 0F | `address`, `quantity`, `byte_count`, `data`, `coils` | `address`, `quantity` |
| 10 | `address`, `quantity`, `byte_count`, `values` | `address`, `quantity` |
| 11 | none | `byte_count`, `server_id`, `run_indicator`, `additional_data` |
| 14 | `byte_count`, `sub_requests` (`reference_type`, `file_number`, `record_number`, `record_length`) | `data_length`, `sub_responses` (`file_response_length`, `reference_type`, `data`) |
| 15 | `byte_count`, `sub_requests` (as 14, plus `data`) | an echo, as `sub_responses` |
| 16 | `address`, `and_mask`, `or_mask` | the same, an echo |
| 17 | `read_address`, `read_quantity`, `write_address`, `write_quantity`, `write_byte_count`, `values` | `byte_count`, `values` |
| 18 | `fifo_address` | `byte_count`, `fifo_count`, `values` |
| 2B / 0E | `mei_type`, `read_device_id_code`, `object_id` | `conformity_level`, `more_follows`, `next_object_id`, `number_of_objects`, `objects` (`id` named, `length`, `value`) |
| 2B / 0D | `control`, `access`, optional fields, `node`, `index`, `subindex`, `start`, `count`, `data` | the same without the request-only options |
| any, with the exception bit | | `function` (the code without the bit), `exception` and its name; for CANopen the extended failure `AB FF len 0D AE`/`CE`: `failure`, `supported_control` or `error_code` with its meaning |
| user-defined or unknown | `data` | `data` |

A user-defined function code (or any code mogbus does not define) prints its payload as `data` and
the direction `unknown`: without the device's definition there is nothing to tell a request from a
response. Force one with `--direction` to see the same bytes under it.

## Detecting the framing

`--framing auto` (the default) checks the bytes against the three formats:

| Format | Detected when |
|---|---|
| ASCII | the frame starts with `:`, ends with CR LF and its LRC matches |
| MBAP | the protocol id is 0 and the length field equals the bytes that follow it |
| RTU | the CRC matches |

When exactly one fits, `framing_reason` says which rule. When none fits, the closest is used (`:` is
ASCII; protocol id 0 with a plausible length is MBAP; anything else RTU) and the damage is listed.
When several fit, which is rare, the output has `ambiguous: true` and a `candidates` list with the full
decoding of each, so nothing is guessed; `--framing` picks one.

## Detecting the direction

A request and a response of the same function code often have the same shape (FC 05, 06, 08, 16 and 15
answer with an echo), and other pairs differ only in a length or a byte count. `--direction auto` decodes the
PDU both ways and keeps the reading that fits: a request has the right length for its function code,
and a response has a byte count that matches its data. Reasons given in `direction_reason`:

| Reason | Meaning |
|---|---|
| the exception bit of the function code is set | an exception answer |
| length and fields fit the request layout, not the response layout | only the request reading is clean |
| length and byte count fit the response layout, not the request layout | only the response reading is clean |
| length and fields fit both a request and a response; assumed request | an echo or an identical shape; `--direction response` shows the other reading |
| neither layout fits cleanly; assumed request | a damaged PDU; with a byte count that matches the length, a response |
| function code is not defined by mogbus, so the payload stays raw | the direction is `unknown` |

A read request with a quantity above the limit and a response with matching byte count are told apart
by the quantity limits, which count as a mismatch in the request reading.

## Damaged frames

A frame that is wrong is still decoded as far as it can be, then the problems are listed and `ok` is
false (JSON: `"ok":false`, `"errors":[...]`). A bad checksum shows `crc_ok: false` and the value it
should have had:

```text
adu:
  address: 1 (0x01)
  crc: 14891 (0x3A2B)
  crc_ok: false
  crc_expected: 63131 (0xF69B)
...
errors:
  - CRC mismatch: the frame has 0x3A2B, the data gives 0xF69B
ok: false
```

What counts as an error (`errors`, exit status 1):

- a wrong CRC or LRC; an ASCII frame without CR LF, with an odd number of hex digits or a character that
  is not a hex digit;
- an MBAP protocol id other than 0, or a length field that does not match the bytes of the frame;
- a PDU that stops before a field ends, as `truncated at byte 13: the register data announces 4 bytes, 2 left`
  (the offset counts bytes of the frame; for ASCII, of the decoded bytes), and bytes left after the last field.

What is a `warnings` entry (still exit status 0): values a server would refuse, such as a quantity
of 0 or above the limit of the function code, a byte count that does not match the quantity, or a
coil value that is not `FF00` or `0000`.

## From C and Python

```c
char text[MOGBUS_DECODE_BUFFER_SIZE];
size_t len;
uint8_t frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A };

if (mogbus_decode_frame(frame, sizeof(frame), MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO,
                        false, text, sizeof(text), &len) != MOGBUS_ERR_INVALID)
	puts(text);
```

```python
import mogbus

print(mogbus.decode_frame("01 03 00 00 00 01 84 0A"))
frame = mogbus.decode_frame(bytes.fromhex("010300000001840A"), as_dict=True)
assert frame["pdu"]["function_name"] == "Read Holding Registers" and frame["ok"]
```

@ref mogbus_decode_frame "mogbus_decode_frame()" needs no context and no server, does not allocate and may
be called from any thread, including a [frame hook](../library/library-hooks.md#watching-and-rewriting-frames).
It returns `MOGBUS_OK`, `MOGBUS_ERR_FRAME` for a malformed frame (the text is complete), `MOGBUS_ERR_NOMEM`
when `out` is too small (the text is cut; `MOGBUS_DECODE_BUFFER_SIZE` always suffices) and
`MOGBUS_ERR_INVALID` for NULL arguments or a frame above 4096 bytes. In Python a damaged frame is a
result, not an exception: check `ok`.

## Limits

- One frame per call, at most 4096 bytes: a capture of a TCP stream has to be split into frames first
  (`tshark` does it).
- Only the function codes mogbus implements are decoded field by field; for the other standard codes
  (such as 0x09 or 0x0D) the PDU is shown raw.
- Only the data is decoded: nothing is checked against a device (an address that does not exist, a
  value out of the point's range), and a request cannot be matched with its response.

## See also

- [Function codes](function-codes.md)
- [Exception codes](exceptions.md)
- [Command line](../command-line.md)
- [Watching and rewriting frames](../library/library-hooks.md#watching-and-rewriting-frames)

# Write File Record (0x15)

Function code 0x15 (decimal 21) writes 16-bit records in the device's configured file storage.

## Syntax

```text
Request:   15 | byte count (1) | sub-request ...
             sub-request: 06 | file number (2) | record number (2) | record length (2) | record data (2 x record length)
Response:  an echo of the request
Exception: 95 | code (1)
```

## Default

No files: every request fails with exception 02.

## Valid in

- file records
- all transports

## Behaviour

- `byte count` is 9 to 251 and equals the total size of all sub-requests; each one is
  `7 + 2 x record length` bytes.
- The reference type must be `0x06`, the file number 1 to 65535, the record number 0 to 9999
  (`0x270F`), the record length at least 1, and the range must end at record 9999 or below.
- A sub-request may write only a contiguous range that the config defines. Files and records cannot
  be created.
- Written values are readable with [Read File Record (0x14)](fc14-read-file-record.md). They live in
  memory: the config file is not changed and the values are lost when the simulator restarts.
- All-or-nothing: every sub-request is validated (shape, ranges, availability) before any record
  changes. On any exception no record is modified.

## Errors

| Code | When |
|---|---|
| 03 | wrong byte count or framing, a truncated or misaligned sub-request, a reference type other than `0x06`, file number 0, record number above `0x270F`, a range past `0x270F`, or record length 0 |
| 02 | a file or any record of a range is not configured |

## Examples

Write two registers to file 1, record 10:

```text
request:   15 0B 06 0001 000A 0002 ABCD EF01
response:  15 0B 06 0001 000A 0002 ABCD EF01
```

```text
request:   14 07 06 0001 000A 0002
response:  14 06 05 06 ABCD EF01
```

## See also

- [Read File Record (0x14)](fc14-read-file-record.md)
- [Writes are all-or-nothing](writes.md)
- [Config reference: file records](../config/device/file.md)

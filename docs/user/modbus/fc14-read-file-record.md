# Read File Record (0x14)

Function code 0x14 (decimal 20) reads 16-bit records from the device's configured file storage.

## Syntax

```text
Request:   14 | byte count (1) | sub-request (7) ...
             sub-request: 06 | file number (2) | record number (2) | record length (2)
Response:  14 | data length (1) | sub-response ...
             sub-response: file response length (1) | 06 | record data (2 x record length)
Exception: 94 | code (1)
```

```scheme
(file FILE_NO (record REC_NO VALUE ...))
```

## Default

No files: every request fails with exception 02.

## Valid in

- file records
- all transports

## Behaviour

- Each sub-request is 7 bytes; `byte count` is their total (7 to 245, a multiple of 7). Several
  sub-requests are answered in order in one response.
- The reference type must be `0x06`, the file number 1 to 65535, the record number 0 to 9999
  (`0x270F`), the record length at least 1, and `record number + length - 1` at most 9999.
- A sub-response is `1 + 2 x record length` as its file response length, then `06`, then the register
  values. `data length` is the total size of all sub-responses.
- File records are separate from the four tables. Only configured `(file ...)` ranges exist, and every
  register of the requested range must be defined by the config. Values are returned as configured
  or as last written by [Write File Record (0x15)](fc15-write-file-record.md).
- All checks (shape, ranges, response capacity, then availability) happen before any data is returned,
  so an error in one sub-request means an exception and no partial answer.

## Errors

| Code | When |
|---|---|
| 03 | wrong byte count or framing, a reference type other than `0x06`, file number 0, record number above `0x270F`, record length 0, a range past `0x270F`, or a response that would not fit in a PDU |
| 02 | a file or any record of a requested range is not configured |

## Examples

With `(file 1 (record 0 #x1234 #x5678))`, read file 1, record 0, two registers:

```text
request:   14 07 06 0001 0000 0002
response:  14 06 05 06 1234 5678
```

```python
from pymodbus.pdu.file_message import FileRecord

# pymodbus takes record_length in bytes: 4 bytes are 2 registers
record = FileRecord(file_number=1, record_number=0, record_length=4)
print(client.read_file_record([record]).records[0].record_data.hex())   # 12345678
```

## See also

- [Write File Record (0x15)](fc15-write-file-record.md)
- [Config reference: file records](../config/device/file.md)

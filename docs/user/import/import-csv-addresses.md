# Address base and Modicon addresses

mogbus addresses are the 0-based numbers sent on the wire. Manuals often count from 1; the importer converts.

## Modicon addresses

If every address is a 5 or 6 digit number whose first digit is 0, 1, 3 or 4 (`40001`, `300017`,
`00005`), they are Modicon numbers: the first digit selects the table (0 coil, 1 discrete input, 3
input register, 4 holding register) and the rest counts from 1, so `40001` is `holding 0` and `30003`
is `input 2`.

If only some addresses look like that, none is treated as Modicon and a warning says so.
`--modicon yes|no|auto` forces the choice.

## Other addresses

`--base 0|1|auto` says what the first address is. With `auto` the base is 1 when the smallest address
in the file is 1 and no address is 0 (with a warning), otherwise 0. `--base 1` subtracts 1 from every
address. For Modicon numbers `--base` applies after the first digit is removed (`auto` is 1).

## Notations

- Hex addresses (`0x10`, `10h`, `$10`) are read as hex.
- A bit address such as `40001.3` is not supported: the row is skipped with a warning.
- A table cell, when present, wins over the Modicon prefix (with a warning if they disagree).

## See also

- [Importing a CSV register map](import-csv.md)
- [Data types, byte layout and values](import-csv-types.md)

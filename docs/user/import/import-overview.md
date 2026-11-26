# Importing a register map

If you already have a register map, do not retype it. `tools/mogbus_import.py` turns a CSV table or a pymodbus simulator file into a mogbus config.

It needs Python 3.9 or newer and nothing else (no pip packages, no mogbus library).

```bash
./tools/mogbus_import.py csv examples/import/meter-map.csv --server tcp --port 1502 -o meter.scm
./tools/mogbus_import.py pymodbus examples/import/pymodbus-setup.json -o plant.scm
./mogbus --check meter.scm
```

The config goes to stdout, or to the file given with `-o`. It starts with a comment saying where it
came from. Every point keeps its name, description and unit as a `(note "...")`.

- **CSV table:** see [Importing a CSV register map](import-csv.md).
- **pymodbus simulator file:** see [Importing a pymodbus simulator file](import-pymodbus.md).

## Warnings and errors

Things the importer cannot map, or is unsure about, are **warnings**. They are printed to stderr and
written into the output as `;` comments, so they stay with the file. Read them: they tell you where the
config differs from the original. Errors (for example two points on the same address) stop the import
and write nothing.

## Options

| Option (both formats) | Meaning |
|---|---|
| `-o FILE` | write the config to `FILE` instead of stdout |
| `--strict` | treat every warning as an error: exit status 1, nothing written |
| `--check` | run `$MOGBUS_BIN` (default `./mogbus`) `--check` on the result; a failing check exits 1 |
| `--unit-id N` | Modbus unit id of the device (default 1) |
| `--name TEXT` | device name (default: the file name, or the pymodbus device name) |

Exit status: 0 success, 1 errors, warnings under `--strict` or a failed `--check`, 2 bad arguments.

## See also

- [Write your own profile](../profiles/write-your-own-profile.md)
- [Config reference](../config/format.md)

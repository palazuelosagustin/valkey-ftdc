# valkey-ftdc

`valkey-ftdc` is an FTDC-style external Valkey module and companion reader.

## Build

```bash
make build
```

Outputs:

- `build/valkey-ftdc.so`
- `build/valkey-ftdcstat`

## Load

```bash
valkey-server \
  --loadmodule /path/to/build/valkey-ftdc.so \
  path /var/lib/valkey/diagnostic.data \
  interval-ms 1000 \
  max-file-mb 64 \
  max-dir-mb 512 \
  collect-host-stats yes
```

## Commands

- `FTDC.STATUS`
- `FTDC.SAMPLE`
- `FTDC.FLUSH`
- `FTDC.ROTATE`
- `FTDC.CONFIG GET [name]`
- `FTDC.CONFIG SET name value`

## File format

Each metrics file is:

```text
VKFTDC1
<metadata-json>
<sample-json>
<sample-json>
...
```

## Reader

```bash
build/valkey-ftdcstat /var/lib/valkey/diagnostic.data
build/valkey-ftdcstat --json /var/lib/valkey/diagnostic.data
build/valkey-ftdcstat --view memory /var/lib/valkey/diagnostic.data
```

## Notes

- Safe defaults: slowlog capture is disabled, and slowlog args are redacted if enabled.
- Linux builds collect host stats from `/proc`; non-Linux builds emit `{"supported":false}` for the host section.

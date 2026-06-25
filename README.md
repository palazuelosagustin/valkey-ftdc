# valkey-ftdc

`valkey-ftdc` is an FTDC-style external Valkey module and companion reader.

## Build

On macOS:

```bash
make all
```

On Linux:

```bash
make all
```

Outputs:

- `build/valkey-ftdc.so`

The module binary is platform-specific:

- build `valkey-ftdc.so` on macOS for macOS
- build `valkey-ftdc.so` on Linux for Linux

Do not copy a macOS-built module to Linux or a Linux-built module to macOS.

## Load

```bash
valkey-server \
  --loadmodule /path/to/build/valkey-ftdc.so \
  path /var/lib/valkey/diagnostic.data \
  interval-ms 1000 \
  delta-metrics yes \
  checkpoint-interval-ms 60000 \
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

New write-path controls:

- `delta-metrics yes|no`: enable format-version 2 checkpoint/delta records
- `checkpoint-interval-ms <ms>`: force a full checkpoint at the configured interval while delta mode is enabled

## Collected Metrics

Each sample includes:

- `ts_ms`
- Valkey `INFO` sections: `server`, `clients`, `memory`, `persistence`, `stats`, `replication`, `cpu`, `commandstats`, and `cluster`
- `LATENCY LATEST` events with `event`, `latest_ms`, `max_ms`, and `all_time_ms`
- `SLOWLOG LEN`; if `collect-slowlog yes` is enabled, up to 8 recent slowlog entries with `id`, `ts`, `duration_usec`, and `args`

If `collect-host-stats yes` is enabled, Linux builds also include host data from `/proc`:

- `loadavg`
- host CPU counters from `/proc/stat`, including `user`, `nice`, `system`, `idle`, `iowait`, `irq`, `softirq`, `steal`, `guest`, `guest_nice`, `ctxt`, `processes`, `procs_running`, and `procs_blocked`
- memory data from `/proc/meminfo`
- disk data from `/proc/diskstats`
- network data from `/proc/net/dev`
- process data from `/proc/self/status` and `/proc/self/io`

## File format

Each metrics file is:

```text
VKFTDC1
<metadata-json>
<sample-json>
<sample-json>
...
```

Rotation produces two file types:

- `metrics.<timestamp>.vkftdc`: the sampled data file. It contains the `VKFTDC1`
  header, one metadata JSON line, and then one JSON record per line.
- `metadata.<timestamp>.json`: a small sidecar JSON file that describes the
  rotated metrics file itself. It is not a stream of samples.

Format version 1 stores raw snapshot samples exactly as collected.

Format version 2 is enabled with `delta-metrics yes` and writes:

- `checkpoint` records with full absolute samples
- `delta` records with per-metric deltas for selected monotonic counters
- `restart` records when the collector detects a segment boundary inside the same process lifetime

The metadata and sidecar both include `format_version`, `schema_version`, and
`delta_mode`. The first record in every new file is always a checkpoint.

## Build On Another Host

To build on a different Linux host or VM, copy the source tree and build there:

```bash
cd ~/bin/valkey-ftdc
tar -czf /tmp/valkey-ftdc-src.tar.gz Makefile README.md go.mod include src tools tests
scp /tmp/valkey-ftdc-src.tar.gz user@host:/tmp/
```

Then on that host:

```bash
rm -rf /tmp/valkey-ftdc
mkdir -p /tmp/valkey-ftdc
tar -C /tmp/valkey-ftdc -xzf /tmp/valkey-ftdc-src.tar.gz
cd /tmp/valkey-ftdc
make all
file build/valkey-ftdc.so
```

On Linux, `file build/valkey-ftdc.so` should report an `ELF 64-bit` shared object.

## License

This project is intended to be released under the MIT License.

`include/valkeymodule.h` is vendored from Valkey, whose source tree is licensed
under the BSD 3-Clause License. Keep the upstream copyright and license notice
for that file when redistributing it.

## Notes

- Safe defaults: slowlog capture is disabled, and slowlog args are redacted if enabled.
- Linux builds collect host stats from `/proc`; non-Linux builds emit `{"supported":false}` for the host section.

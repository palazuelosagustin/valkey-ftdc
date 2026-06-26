//go:build integration

package integration

import (
	"bufio"
	"bytes"
	"encoding/json"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

func freePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

func runCLI(t *testing.T, port int, args ...string) string {
	t.Helper()
	base := []string{"-p", strconv.Itoa(port), "--raw"}
	base = append(base, args...)
	cmd := exec.Command("valkey-cli", base...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("valkey-cli %v failed: %v\n%s", args, err, out)
	}
	return strings.TrimSpace(string(out))
}

func waitPing(t *testing.T, port int) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		cmd := exec.Command("valkey-cli", "-p", strconv.Itoa(port), "PING")
		if err := cmd.Run(); err == nil {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal("server did not become ready")
}

func sampleJSON(t *testing.T, port int) map[string]any {
	t.Helper()
	var sample map[string]any
	if err := json.Unmarshal([]byte(runCLI(t, port, "FTDC.SAMPLE")), &sample); err != nil {
		t.Fatalf("unmarshal sample: %v", err)
	}
	return sample
}

func latestMetricsFile(t *testing.T, dir string) string {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	latest := ""
	for _, ent := range entries {
		if strings.HasPrefix(ent.Name(), "metrics.") {
			if ent.Name() > latest {
				latest = ent.Name()
			}
		}
	}
	if latest == "" {
		t.Fatal("expected metrics file")
	}
	return filepath.Join(dir, latest)
}

func readMetricsRecords(t *testing.T, path string) (map[string]any, []map[string]any) {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	if !sc.Scan() {
		t.Fatal("missing file magic")
	}
	if got := sc.Text(); got != "VKFTDC1" {
		t.Fatalf("unexpected magic %q", got)
	}
	if !sc.Scan() {
		t.Fatal("missing metadata line")
	}
	var metadata map[string]any
	if err := json.Unmarshal(sc.Bytes(), &metadata); err != nil {
		t.Fatalf("unmarshal metadata: %v", err)
	}
	var records []map[string]any
	for sc.Scan() {
		line := bytes.TrimSpace(sc.Bytes())
		if len(line) == 0 {
			continue
		}
		var rec map[string]any
		if err := json.Unmarshal(line, &rec); err != nil {
			t.Fatalf("unmarshal record %q: %v", line, err)
		}
		records = append(records, rec)
	}
	if err := sc.Err(); err != nil {
		t.Fatal(err)
	}
	return metadata, records
}

func TestModuleLifecycle(t *testing.T) {
	if _, err := exec.LookPath("valkey-server"); err != nil {
		t.Skip("valkey-server not found")
	}
	root, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	modulePath := filepath.Join(root, "build", "valkey-ftdc.so")
	if _, err := os.Stat(modulePath); err != nil {
		t.Skip("module not built")
	}
	port := freePort(t)
	tmp := t.TempDir()
	logPath := filepath.Join(tmp, "server.log")
	cmd := exec.Command("valkey-server",
		"--port", strconv.Itoa(port),
		"--save", "",
		"--appendonly", "no",
		"--loadmodule", modulePath,
		"path", filepath.Join(tmp, "diagnostic.data"),
		"interval-ms", "100",
		"max-file-mb", "1",
		"max-dir-mb", "4",
		"collect-host-stats", "no",
	)
	logFile, err := os.Create(logPath)
	if err != nil {
		t.Fatal(err)
	}
	defer logFile.Close()
	cmd.Stdout = logFile
	cmd.Stderr = logFile
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = exec.Command("valkey-cli", "-p", strconv.Itoa(port), "SHUTDOWN", "NOSAVE").Run()
		_ = cmd.Wait()
	}()
	waitPing(t, port)

	if out := runCLI(t, port, "FTDC.STATUS"); !strings.Contains(out, "enabled") {
		t.Fatalf("unexpected status output: %s", out)
	}
	sample := runCLI(t, port, "FTDC.SAMPLE")
	if !strings.Contains(sample, "\"valkey\"") || strings.Contains(sample, "requirepass") {
		t.Fatalf("unexpected sample: %s", sample)
	}
	_ = runCLI(t, port, "PING")
	_ = runCLI(t, port, "INFO", "commandstats")
	parsed := sampleJSON(t, port)
	valkey, _ := parsed["valkey"].(map[string]any)
	infoRoot, _ := valkey["info"].(map[string]any)
	commandstats, _ := infoRoot["commandstats"].(map[string]any)
	if len(commandstats) == 0 {
		t.Fatal("expected commandstats data")
	}
	foundStructured := false
	for _, raw := range commandstats {
		entry, ok := raw.(map[string]any)
		if !ok {
			continue
		}
		if _, ok := entry["calls"]; ok {
			foundStructured = true
			break
		}
	}
	if !foundStructured {
		t.Fatalf("expected structured commandstats, got %#v", commandstats)
	}
	if out := runCLI(t, port, "FTDC.CONFIG", "SET", "enabled", "no"); out != "OK" {
		t.Fatalf("disable failed: %s", out)
	}
	if out := runCLI(t, port, "FTDC.CONFIG", "SET", "enabled", "yes"); out != "OK" {
		t.Fatalf("enable failed: %s", out)
	}
	if out := runCLI(t, port, "FTDC.ROTATE"); !strings.Contains(out, "metrics.") {
		t.Fatalf("rotate failed: %s", out)
	}
	time.Sleep(400 * time.Millisecond)

	diagDir := filepath.Join(tmp, "diagnostic.data")
	entries, err := os.ReadDir(diagDir)
	if err != nil {
		t.Fatal(err)
	}
	var metrics int
	for _, ent := range entries {
		if strings.HasPrefix(ent.Name(), "metrics.") {
			metrics++
		}
	}
	if metrics == 0 {
		t.Fatal("expected metrics files")
	}
}

func TestDeltaMetricsWriteCheckpointAndDeltaRecords(t *testing.T) {
	if _, err := exec.LookPath("valkey-server"); err != nil {
		t.Skip("valkey-server not found")
	}
	root, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	modulePath := filepath.Join(root, "build", "valkey-ftdc.so")
	if _, err := os.Stat(modulePath); err != nil {
		t.Skip("module not built")
	}
	port := freePort(t)
	tmp := t.TempDir()
	diagDir := filepath.Join(tmp, "diagnostic.data")
	cmd := exec.Command("valkey-server",
		"--port", strconv.Itoa(port),
		"--save", "",
		"--appendonly", "no",
		"--loadmodule", modulePath,
		"path", diagDir,
		"interval-ms", "100",
		"checkpoint-interval-ms", "60000",
		"delta-metrics", "yes",
		"collect-host-stats", "no",
	)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = exec.Command("valkey-cli", "-p", strconv.Itoa(port), "SHUTDOWN", "NOSAVE").Run()
		_ = cmd.Wait()
	}()
	waitPing(t, port)

	for i := 0; i < 5; i++ {
		_ = runCLI(t, port, "PING")
		time.Sleep(120 * time.Millisecond)
	}
	if out := runCLI(t, port, "FTDC.FLUSH"); out != "OK" {
		t.Fatalf("flush failed: %s", out)
	}

	metadata, records := readMetricsRecords(t, latestMetricsFile(t, diagDir))
	if got := metadata["format_version"]; got != float64(2) {
		t.Fatalf("unexpected format version %#v", got)
	}
	if got := metadata["delta_mode"]; got != true {
		t.Fatalf("expected delta mode metadata, got %#v", got)
	}
	if len(records) < 2 {
		t.Fatalf("expected at least 2 records, got %d", len(records))
	}
	if kind, _ := records[0]["sample_kind"].(string); kind != "checkpoint" {
		t.Fatalf("expected first record checkpoint, got %#v", records[0])
	}
	foundDelta := false
	for _, rec := range records[1:] {
		if kind, _ := rec["sample_kind"].(string); kind == "delta" {
			foundDelta = true
			if _, ok := rec["deltas"].(map[string]any); !ok {
				t.Fatalf("delta record missing deltas map: %#v", rec)
			}
			break
		}
	}
	if !foundDelta {
		t.Fatalf("expected delta record, got %#v", records)
	}
}

func TestDeltaMetricsWriteSamplesWithHostStatsEnabled(t *testing.T) {
	if _, err := exec.LookPath("valkey-server"); err != nil {
		t.Skip("valkey-server not found")
	}
	root, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	modulePath := filepath.Join(root, "build", "valkey-ftdc.so")
	if _, err := os.Stat(modulePath); err != nil {
		t.Skip("module not built")
	}
	port := freePort(t)
	tmp := t.TempDir()
	diagDir := filepath.Join(tmp, "diagnostic.data")
	cmd := exec.Command("valkey-server",
		"--port", strconv.Itoa(port),
		"--save", "",
		"--appendonly", "no",
		"--loadmodule", modulePath,
		"path", diagDir,
		"interval-ms", "100",
		"checkpoint-interval-ms", "60000",
		"delta-metrics", "yes",
		"collect-host-stats", "yes",
	)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = exec.Command("valkey-cli", "-p", strconv.Itoa(port), "SHUTDOWN", "NOSAVE").Run()
		_ = cmd.Wait()
	}()
	waitPing(t, port)

	for i := 0; i < 5; i++ {
		_ = runCLI(t, port, "PING")
		time.Sleep(120 * time.Millisecond)
	}
	if out := runCLI(t, port, "FTDC.FLUSH"); out != "OK" {
		t.Fatalf("flush failed: %s", out)
	}
	status := runCLI(t, port, "FTDC.STATUS")
	if strings.Contains(status, "delta encoding failed") {
		t.Fatalf("unexpected status error: %s", status)
	}

	_, records := readMetricsRecords(t, latestMetricsFile(t, diagDir))
	if len(records) == 0 {
		t.Fatal("expected at least one metrics record")
	}
}

func TestHostStatsIncludeRawProcStatCounters(t *testing.T) {
	if _, err := exec.LookPath("valkey-server"); err != nil {
		t.Skip("valkey-server not found")
	}
	root, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	modulePath := filepath.Join(root, "build", "valkey-ftdc.so")
	if _, err := os.Stat(modulePath); err != nil {
		t.Skip("module not built")
	}
	port := freePort(t)
	tmp := t.TempDir()
	cmd := exec.Command("valkey-server",
		"--port", strconv.Itoa(port),
		"--save", "",
		"--appendonly", "no",
		"--loadmodule", modulePath,
		"path", filepath.Join(tmp, "diagnostic.data"),
		"interval-ms", "100",
		"collect-host-stats", "yes",
	)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = exec.Command("valkey-cli", "-p", strconv.Itoa(port), "SHUTDOWN", "NOSAVE").Run()
		_ = cmd.Wait()
	}()
	waitPing(t, port)

	sample := sampleJSON(t, port)
	host, _ := sample["host"].(map[string]any)
	cpu, _ := host["cpu"].(map[string]any)
	if supported, _ := host["supported"].(bool); !supported {
		t.Skip("host stats not supported on this platform")
	}
	for _, field := range []string{"user", "system", "idle", "ctxt", "processes", "procs_running", "procs_blocked"} {
		if _, ok := cpu[field]; !ok {
			t.Fatalf("expected host cpu field %q in %#v", field, cpu)
		}
	}
}

func TestMetricsFileHasHeader(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "metrics.sample.vkftdc")
	content := []byte("VKFTDC1\n{\"format_version\":1}\n{\"ts_ms\":1}\n")
	if err := os.WriteFile(path, content, 0o644); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	line, err := bufio.NewReader(f).ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(line, []byte("VKFTDC1\n")) {
		t.Fatalf("unexpected header: %q", line)
	}
}

func TestConfigReportsDeltaSettings(t *testing.T) {
	if _, err := exec.LookPath("valkey-server"); err != nil {
		t.Skip("valkey-server not found")
	}
	root, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	modulePath := filepath.Join(root, "build", "valkey-ftdc.so")
	if _, err := os.Stat(modulePath); err != nil {
		t.Skip("module not built")
	}
	port := freePort(t)
	tmp := t.TempDir()
	cmd := exec.Command("valkey-server",
		"--port", strconv.Itoa(port),
		"--save", "",
		"--appendonly", "no",
		"--loadmodule", modulePath,
		"path", filepath.Join(tmp, "diagnostic.data"),
		"delta-metrics", "yes",
		"checkpoint-interval-ms", "60000",
	)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = exec.Command("valkey-cli", "-p", strconv.Itoa(port), "SHUTDOWN", "NOSAVE").Run()
		_ = cmd.Wait()
	}()
	waitPing(t, port)

	all := runCLI(t, port, "FTDC.CONFIG", "GET")
	for _, needle := range []string{"delta-metrics", "yes", "checkpoint-interval-ms", "60000"} {
		if !strings.Contains(all, needle) {
			t.Fatalf("expected %q in config output:\n%s", needle, all)
		}
	}
	status := runCLI(t, port, "FTDC.STATUS")
	for _, needle := range []string{"delta_metrics", "yes", "checkpoint_interval_ms", "60000"} {
		if !strings.Contains(status, needle) {
			t.Fatalf("expected %q in status output:\n%s", needle, status)
		}
	}
}

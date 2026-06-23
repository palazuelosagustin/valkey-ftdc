package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type sample struct {
	TSMS   int64                  `json:"ts_ms"`
	Valkey map[string]any         `json:"valkey"`
	Host   map[string]any         `json:"host"`
	Raw    map[string]any         `json:"-"`
}

func main() {
	view := flag.String("view", "summary", "summary|memory|clients|cpu|persistence|replication|commandstats|host")
	jsonOut := flag.Bool("json", false, "emit JSON")
	flag.Parse()

	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: valkey-ftdcstat [--json] [--view name] <diagnostic.data>")
		os.Exit(2)
	}

	samples, err := readSamples(flag.Arg(0))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if len(samples) == 0 {
		fmt.Fprintln(os.Stderr, "no samples found")
		os.Exit(1)
	}

	if *jsonOut {
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		_ = enc.Encode(samples)
		return
	}

	if err := renderView(os.Stdout, *view, samples); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func readSamples(root string) ([]sample, error) {
	entries, err := os.ReadDir(root)
	if err != nil {
		return nil, err
	}
	var files []string
	for _, ent := range entries {
		if ent.IsDir() {
			continue
		}
		if strings.HasPrefix(ent.Name(), "metrics.") && strings.HasSuffix(ent.Name(), ".vkftdc") {
			files = append(files, filepath.Join(root, ent.Name()))
		}
	}
	sort.Strings(files)
	var out []sample
	for _, path := range files {
		part, err := readSamplesFile(path)
		if err != nil {
			return nil, err
		}
		out = append(out, part...)
	}
	return out, nil
}

func readSamplesFile(path string) ([]sample, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
	if !sc.Scan() {
		return nil, errors.New("empty metrics file")
	}
	if sc.Text() != "VKFTDC1" {
		return nil, fmt.Errorf("unexpected magic in %s", path)
	}
	if !sc.Scan() {
		return nil, fmt.Errorf("missing metadata in %s", path)
	}
	var samples []sample
	for sc.Scan() {
		line := sc.Bytes()
		var raw map[string]any
		if err := json.Unmarshal(line, &raw); err != nil {
			continue
		}
		var s sample
		s.Raw = raw
		if v, ok := raw["ts_ms"].(float64); ok {
			s.TSMS = int64(v)
		}
		if v, ok := raw["valkey"].(map[string]any); ok {
			s.Valkey = v
		}
		if v, ok := raw["host"].(map[string]any); ok {
			s.Host = v
		}
		samples = append(samples, s)
	}
	if err := sc.Err(); err != nil && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return samples, nil
}

func renderView(w io.Writer, view string, samples []sample) error {
	last := samples[len(samples)-1]
	switch view {
	case "summary":
		fmt.Fprintf(w, "samples: %d\n", len(samples))
		fmt.Fprintf(w, "last_ts_ms: %d\n", last.TSMS)
		printInfoFields(w, last, "server", []string{"valkey_version", "tcp_port", "uptime_in_seconds"})
		printInfoFields(w, last, "memory", []string{"used_memory", "used_memory_human", "maxmemory"})
		printInfoFields(w, last, "stats", []string{"total_connections_received", "total_commands_processed"})
		return nil
	case "memory", "clients", "cpu", "persistence", "replication", "commandstats":
		printSection(w, last, view)
		return nil
	case "host":
		b, _ := json.MarshalIndent(last.Host, "", "  ")
		fmt.Fprintln(w, string(b))
		return nil
	default:
		return fmt.Errorf("unknown view %q", view)
	}
}

func printSection(w io.Writer, s sample, section string) {
	info := infoSection(s, section)
	b, _ := json.MarshalIndent(info, "", "  ")
	fmt.Fprintln(w, string(b))
}

func printInfoFields(w io.Writer, s sample, section string, fields []string) {
	info := infoSection(s, section)
	for _, field := range fields {
		if val, ok := info[field]; ok {
			fmt.Fprintf(w, "%s.%s: %v\n", section, field, val)
		}
	}
}

func infoSection(s sample, section string) map[string]any {
	infoRoot, _ := s.Valkey["info"].(map[string]any)
	info, _ := infoRoot[section].(map[string]any)
	if info == nil {
		return map[string]any{}
	}
	return info
}

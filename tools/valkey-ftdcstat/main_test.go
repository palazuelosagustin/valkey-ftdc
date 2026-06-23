package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestReadSamples(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "metrics.2026-06-20T12-00-00Z.vkftdc")
	content := "VKFTDC1\n{\"format_version\":1}\n{\"ts_ms\":1,\"valkey\":{\"info\":{\"memory\":{\"used_memory\":1}}},\"host\":{\"enabled\":false}}\n"
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	samples, err := readSamples(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(samples) != 1 {
		t.Fatalf("expected 1 sample, got %d", len(samples))
	}
	if samples[0].TSMS != 1 {
		t.Fatalf("expected ts 1, got %d", samples[0].TSMS)
	}
}

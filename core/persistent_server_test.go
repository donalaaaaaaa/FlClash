//go:build !cgo && !android

package core

import (
	"errors"
	"reflect"
	"testing"
	"time"
)

func TestRunPersistentServerRetriesDialAndReconnects(t *testing.T) {
	first := &fakeConn{}
	second := &fakeConn{}
	var attempts []string
	var delays []time.Duration
	var served []ipcConn
	steps := 0

	runPersistentServer(
		`\\.\pipe\flclash`,
		func(arg string) (ipcConn, error) {
			attempts = append(attempts, arg)
			switch len(attempts) {
			case 1:
				return nil, errors.New("host not ready")
			case 2:
				return first, nil
			default:
				return second, nil
			}
		},
		func(delay time.Duration) { delays = append(delays, delay) },
		func(conn ipcConn) {
			served = append(served, conn)
			steps++
		},
		func() bool { return steps < 2 },
	)

	if !reflect.DeepEqual(attempts, []string{`\\.\pipe\flclash`, `\\.\pipe\flclash`, `\\.\pipe\flclash`}) {
		t.Fatalf("dial args = %#v, want the same pipe on every attempt", attempts)
	}
	if !reflect.DeepEqual(served, []ipcConn{first, second}) {
		t.Fatalf("served connections = %#v, want both connections", served)
	}
	if len(delays) != 3 {
		t.Fatalf("sleep calls = %d, want one after each failed/disconnected attempt", len(delays))
	}
	for _, delay := range delays {
		if delay != persistentRetryDelay {
			t.Errorf("retry delay = %s, want %s", delay, persistentRetryDelay)
		}
	}
}

func TestRunPersistentServerStopsWithoutAnotherDial(t *testing.T) {
	dials := 0
	serves := 0
	runPersistentServer(
		"pipe",
		func(string) (ipcConn, error) {
			dials++
			return &fakeConn{}, nil
		},
		func(time.Duration) {},
		func(ipcConn) { serves++ },
		func() bool { return serves == 0 },
	)

	if dials != 1 || serves != 1 {
		t.Fatalf("dials = %d, serves = %d, want one bounded iteration", dials, serves)
	}
}

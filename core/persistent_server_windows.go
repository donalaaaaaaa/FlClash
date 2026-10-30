//go:build windows

package core

import "time"

// StartPersistentServer keeps the Core alive while the Windows desktop host
// disconnects and reconnects to the same named pipe.
func StartPersistentServer(arg string) {
	defer releaseOnExit()
	runPersistentServer(arg, dial, time.Sleep, serve, func() bool { return true })
}

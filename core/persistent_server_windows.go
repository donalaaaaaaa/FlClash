//go:build windows

package core

import "time"

// StartPersistentServer keeps the Core alive while the Windows desktop host
// disconnects and reconnects to the same named pipe.
func StartPersistentServer(arg string) {
	runPersistentServer(arg, func(path string) (ipcConn, error) {
		return dial(path)
	}, time.Sleep, servePersistentConnection, func() bool { return true })
}

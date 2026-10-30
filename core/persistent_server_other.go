//go:build !windows && !android

package core

// StartPersistentServer is named for the desktop entrypoint but intentionally
// retains the original one-shot behavior on non-Windows platforms.
func StartPersistentServer(arg string) {
	StartServer(arg)
}

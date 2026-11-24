package ai

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

func GetAvailableTracks(dir string) []string {
	var tracks []string
	err := filepath.WalkDir(dir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			return nil
		}
		ext := strings.ToLower(filepath.Ext(path))
		if ext != ".mp3" && ext != ".flac" && ext != ".wav" && ext != ".m4a" && ext != ".ogg" {
			return nil
		}

		rel, err := filepath.Rel(dir, path)
		if err != nil {
			return nil
		}
		tracks = append(tracks, filepath.ToSlash(rel))
		return nil
	})
	if err != nil {
		return nil
	}
	return tracks
}

func StreamMP3(ctx context.Context, filePath string, onChunk func([]byte) error) error {
	cmd := exec.CommandContext(ctx, "ffmpeg",
		"-re",
		"-i", filePath,
		"-f", "s16le",
		"-ar", "24000",
		"-ac", "1",
		"-loglevel", "quiet",
		"pipe:1",
	)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("failed to start ffmpeg: %w", err)
	}

	buf := make([]byte, 1024)
	for {
		select {
		case <-ctx.Done():
			cmd.Process.Kill()
			return ctx.Err()
		default:
		}

		n, err := io.ReadFull(stdout, buf)
		if n > 0 {
			if chunkErr := onChunk(buf[:n]); chunkErr != nil {
				cmd.Process.Kill()
				return chunkErr
			}
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			break
		}
		if err != nil {
			cmd.Process.Kill()
			return err
		}
	}

	return cmd.Wait()
}

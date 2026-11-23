package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strings"
)

func ensureEnv() error {
	file, err := os.ReadFile(".env")
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	data := strings.SplitSeq(string(file), "\n")
	for line := range data {
		curr := strings.TrimSpace(line)
		if strings.HasPrefix(curr, "#") || len(curr) == 0 {
			continue
		}
		key, value, found := strings.Cut(curr, "=")
		if !found {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.Trim(strings.TrimSpace(value), `"'`)
		if err := os.Setenv(key, value); err != nil {
			return err
		}
	}

	if os.Getenv("GEMINI_API_KEY") == "" {
		fmt.Println("=== Welcome to DeskAI First-Time Setup ===\nNo Gemini API key found. Let's set it up!")

		fmt.Println("Paste your Gemini Api key: ")
		reader := bufio.NewReader(os.Stdin)
		api, err := reader.ReadString('\n')
		if err != nil {
			return err
		}
		api = strings.TrimSpace(api)
		if api == "" {
			fmt.Println("empty api key")
			return errors.New("Api key empty")
		}

		cleanEnv := fmt.Sprintf(
			"GEMINI_API_KEY=%s\nSTT_PROVIDER=gemini\nTTS_PROVIDER=gemini\n",
			api,
		)
		os.WriteFile(".env", []byte(cleanEnv), 0600)
		os.Setenv("GEMINI_API_KEY", api)
		os.Setenv("STT_PROVIDER", "gemini")
		os.Setenv("TTS_PROVIDER", "gemini")

	}

	return nil
}

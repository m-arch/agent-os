#!/usr/bin/bash

# Start LLM server
llama-server -m ~/downloads/Qwen3-Coder-30B-A3B-Instruct-Q3_K_M.gguf -ngl 99 -c 68192 --port 9090 > /dev/null 2>&1 &
LLM_PID=$!

# Wait for server
sleep 5

# Check if server is running
if ! curl -s http://localhost:9090/health > /dev/null 2>&1; then
    exit 1
fi

# Start agent
agent

# Cleanup
kill $LLM_PID 2>/dev/null

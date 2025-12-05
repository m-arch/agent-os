#!/bin/bash
# Claude Code agent wrapper for transcript processing
# Uses claude CLI in print mode for automated responses

WORKSPACE="/root/workspace"

# Read input line by line
while IFS= read -r line; do
    # Skip empty lines
    [ -z "$line" ] && continue

    # Handle special commands
    case "$line" in
        exit|quit)
            echo "Goodbye!"
            exit 0
            ;;
        clear|reset)
            echo "[Context cleared]"
            continue
            ;;
    esac

    # Extract project path if specified
    PROJECT_DIR=""
    if [[ "$line" =~ \[PROJECT:\ *([^\]]+)\] ]]; then
        PROJECT_DIR="${BASH_REMATCH[1]}"
        # Change to project directory for claude context
        if [ -d "$PROJECT_DIR" ]; then
            cd "$PROJECT_DIR"
        fi
    fi

    echo "[Claude processing...]"

    # Run claude in print mode with the prompt
    claude -p "$line" 2>&1

    echo ""
    echo "[Done]"
done

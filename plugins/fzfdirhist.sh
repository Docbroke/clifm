#!/bin/sh

# Directory history plugin via FZF for CliFM
# Written by L. Abramovich
# License GPL2+

if [ -n "$1" ] && { [ "$1" = "--help" ] || [ "$1" = "help" ]; }; then
	name="$(basename "$0")"
	printf "Navigate and execute CliFM directory history via FZF\n"
	printf "Usage: %s\n" "$name"
	exit 0
fi

if ! [ "$(which fzf 2>/dev/null)" ]; then
	printf "CliFM: fzf: Command not found" >&2
	exit 1
fi

FILE="${XDG_CONFIG_HOME:=$HOME/.config}/clifm/profiles/$CLIFM_PROFILE/dirhist.cfm"

sort -u "$FILE" | fzf --prompt="CliFM > " > "$CLIFM_BUS"
#cat "$CLIFM_BUS"
printf "\n"

exit 0

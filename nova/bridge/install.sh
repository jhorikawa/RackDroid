#!/bin/bash
# Put the chat's bridge into Termux on the Nova, from the Mac.
#
#   nova/bridge/install.sh
#
# Needs: Termux (the GitHub build, which adb may write into) with proot-distro
# and a "debian" container that has python3 and a signed-in claude, and
# RackDroid with its chat link on (START > Chat link), which is what makes the
# token this copies. Then, in Termux: bash ./nova-chat.sh
set -euo pipefail

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
APP="${NOVA_APP:-org.rackdroid.test}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LINK="/sdcard/Android/data/$APP/files/chat-link.json"

if ! "$ADB" shell "[ -f '$LINK' ]"; then
	echo "install: RackDroid has no chat-link.json yet. Switch START > Chat link on." >&2
	exit 1
fi

# Through the shell's hands only: never onto shared storage.
into_termux() { "$ADB" shell "run-as com.termux sh -c 'umask 077; cat > files/home/$1'"; }

"$ADB" shell "run-as com.termux mkdir -p files/home/nova-chat"
into_termux nova-chat/nova_bridge.py < "$HERE/nova_bridge.py"
"$ADB" shell "cat '$LINK'" | into_termux nova-chat/chat-link.json
into_termux nova-chat.sh < "$HERE/nova-chat.sh"
echo "Installed. In Termux: bash ./nova-chat.sh"

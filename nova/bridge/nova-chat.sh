# In Termux: what answers RackDroid's chat. Leave it running.
# NOVA_CHAT_MODEL (opus, sonnet, haiku) and NOVA_CHAT_EFFORT choose what answers.
exec proot-distro login debian --bind "$HOME/nova-chat:/nova-chat" -- \
	env NOVA_CHAT_LINK=/nova-chat/chat-link.json ${NOVA_CHAT_MODEL:+NOVA_CHAT_MODEL="$NOVA_CHAT_MODEL"} \
	${NOVA_CHAT_EFFORT:+NOVA_CHAT_EFFORT="$NOVA_CHAT_EFFORT"} python3 /nova-chat/nova_bridge.py run

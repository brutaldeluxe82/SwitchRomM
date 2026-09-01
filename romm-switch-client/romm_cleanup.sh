# TicromM - SD state cleanup for fresh retest.
# Run from Sphaira/hbmenu script runner, or delete these files manually via
# any file browser. Removes cached credentials and library state; keeps
# roms/saves/bios media folders untouched.

# Paired device token (forces fresh device-code pairing)
rm -f /switch/TicromM/device_token.json
# Save-sync state (per-file sync history)
rm -f /switch/TicromM/save_sync_state.json
# Persisted download queue snapshot
rm -f /switch/TicromM/queue_state.json
# Staged update pointer + payload
rm -f /switch/TicromM/update_pending.txt
# Basic-auth config from earlier manual testing (server_url/username/password)
# NOTE: deleting config.json also drops output_layout / timeouts. The app
# recreates it with defaults on next launch; set server URL in the new
# Server & Auth screen afterwards.
rm -f /switch/TicromM/config.json
# Old env-style config if present
rm -f /switch/TicromM/config.env
# App log
rm -f /switch/TicromM/log.txt
rm -f /switch/TicromM/log.txt.1
# Pre-rebrand app data (old RomM Switch Client dir); delete only when the
# migration to /switch/TicromM is confirmed complete.
rm -f /switch/romm_switch_client/device_token.json
rm -f /switch/romm_switch_client/save_sync_state.json
rm -f /switch/romm_switch_client/queue_state.json
rm -f /switch/romm_switch_client/update_pending.txt
rm -f /switch/romm_switch_client/config.json
rm -f /switch/romm_switch_client/config.env
rm -f /switch/romm_switch_client/log.txt
rm -f /switch/romm_switch_client/log.txt.1
echo "TicromM SD cleanup complete."

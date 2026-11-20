#!/bin/bash
# GBL link, Jetson side (run by gbl-link.service, as root).
#
# - Saves the live GBL log to $GBL_LOG_DIR/gbl.log, one timestamped line each.
# - At start-up asks the GBL for its stored log ("L") and saves it to
#   $GBL_LOG_DIR/gbl_stored.log: this recovers what happened while the Jetson
#   was off, e.g. a failed pre-charge.
# - On "GBL:SHUTDOWN" (power switch turned OFF) replies "JETSON:ACK" and halts.
#   The GBL keeps the line powered for 20 s, then opens it.
# - On "GBL:RELAY_WELDED" (the GBL found the relay contacts welded) replies
#   "JETSON:ACK", runs $GBL_MOTORS_OFF_CMD (robot command that disables all
#   motors over CAN; max 5 s) and halts.
#
# Protocol strings must match main/gbl_run.c and components/gbl_log/gbl_log.c.

PORT=${GBL_PORT:-/dev/ttyTHS0}
DIR=${GBL_LOG_DIR:-/var/log/gbl}

mkdir -p "$DIR"
stty -F "$PORT" 115200 raw -echo || exit 1
exec 3<>"$PORT"

printf 'L\n' >&3
stored=0

while IFS= read -r line <&3; do
	line=${line%$'\r'}
	case "$line" in
	"GBL:SHUTDOWN")
		printf 'JETSON:ACK\n' >&3
		echo "$(date '+%F %T') shutdown requested by the GBL power switch" >>"$DIR/gbl.log"
		systemctl poweroff
		;;
	"GBL:RELAY_WELDED")
		# The GBL cannot open the line: cut the motor current first, so the
		# battery can then be unplugged with only the driver logic (~1 A) flowing.
		printf 'JETSON:ACK\n' >&3
		echo "$(date '+%F %T') RELAY WELDED reported by the GBL: motors off, halting" >>"$DIR/gbl.log"
		[ -n "$GBL_MOTORS_OFF_CMD" ] && timeout 5 sh -c "$GBL_MOTORS_OFF_CMD"
		systemctl poweroff
		;;
	"=== stored log (older first) ===")
		stored=1
		: >"$DIR/gbl_stored.log"
		;;
	"=== end of stored log ===")
		stored=0
		;;
	*)
		if [ "$stored" = 1 ]; then
			echo "$line" >>"$DIR/gbl_stored.log"
		else
			echo "$(date '+%F %T') $line" >>"$DIR/gbl.log"
		fi
		;;
	esac
done

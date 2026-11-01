#!/usr/bin/env bash
# Starts a throwaway Haiku test VM for msga on air/OS (VNC 127.0.0.1:5913, SSH 2234).
#
# It boots TasAmp's Haiku R1/beta6 disk (apps/tasamp/.vm/work.qcow2, override
# with MSGA_VM_BASE) read-only through a qcow2 overlay in MSGA_VM_DIR (default
# /mnt/HaikuWork/tmp/msga-vm), and uses TasAmp's SSH key. QEMU locks the
# base: TasAmp's, Snap's, WordCraft's and GridCraft's VMs can't run at the same time.
#
# The sound card (intel-hda) is there for the Media Kit: without one
# media_addon_server crashes when a BSoundPlayer connects.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
TASAMP=$(cd "$ROOT/.." && pwd)/tasamp/.vm
BASE=${MSGA_VM_BASE:-$TASAMP/work.qcow2}
DIR=${MSGA_VM_DIR:-/mnt/HaikuWork/tmp/msga-vm}
OVERLAY=$DIR/overlay.qcow2
STAMP=$DIR/base.stamp
mkdir -p "$DIR"
if [[ -f $DIR/qemu.pid ]] && kill -0 "$(cat "$DIR/qemu.pid")" 2>/dev/null; then
	echo 'The msga test VM is already running.'
	exit 0
fi
stamp=$(stat -c '%s %Y' "$BASE")
if [[ -f $OVERLAY && "$(cat "$STAMP" 2>/dev/null)" != "$stamp" ]]; then
	echo 'Base disk changed; starting from a fresh overlay.'
	rm -f "$OVERLAY"
fi
if [[ ! -f $OVERLAY ]]; then
	qemu-img create -q -f qcow2 -b "$BASE" -F qcow2 "$OVERLAY"
	echo "$stamp" > "$STAMP"
fi
[[ -f $DIR/id_ed25519 ]] || cp "$TASAMP/id_ed25519" "$DIR/id_ed25519"
chmod 600 "$DIR/id_ed25519"
rm -f "$DIR/qmp.sock"
exec qemu-system-x86_64 -enable-kvm -cpu host -m "${MSGA_VM_MEMORY:-6144}" \
	-smp "${MSGA_VM_CPUS:-8}" \
	-drive file="$OVERLAY",format=qcow2,if=ide,index=0 \
	-nic user,model=e1000,hostfwd=tcp:127.0.0.1:2234-:22 \
	-device qemu-xhci -device usb-tablet -vga std -display none \
	-audiodev none,id=snd -device intel-hda -device hda-duplex,audiodev=snd \
	-vnc 127.0.0.1:13 -qmp unix:"$DIR/qmp.sock",server=on,wait=off \
	-pidfile "$DIR/qemu.pid" -serial file:"$DIR/serial.log" -daemonize

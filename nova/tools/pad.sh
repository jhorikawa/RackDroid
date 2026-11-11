# Real controller input for testing, written straight to the Nova's input node.
#
# `adb shell input keyevent` cannot hold a button or move a stick, and its
# press and release land together -- which is how a menu that closed on the
# release of the button that opened it got past testing. These go through the
# kernel like the real buttons do (the D-pad as the HAT axes it really is).
#
#   adb push nova/tools/pad.sh /data/local/tmp/pad.sh
#   adb shell '. /data/local/tmp/pad.sh; key 310 1; tap_hat 1 0; key 310 0'
#
# Key codes: A 304, B 305, X 307, Y 308, L1 310, R1 311, L2 312, R2 313,
# SELECT 314, START 315, L3 317, R3 318. (Which printed letter each of the
# face buttons is depends on the Nova's layout setting.)
# Axes: ABS_X 0, ABS_Y 1 (left stick), ABS_Z 2, ABS_RZ 5 (right stick),
# ABS_GAS 9 (R2), ABS_BRAKE 10 (L2); sticks -32767..32767, triggers 0..32767.
D=/dev/input/event7
syn() { sendevent $D 0 0 0; }
key() { sendevent $D 1 $1 $2; syn; }          # code, 1=down 0=up
hat() { sendevent $D 3 16 $1; sendevent $D 3 17 $2; syn; }   # x, y  (-1/0/1)
tap_hat() { hat $1 $2; sleep 0.12; hat 0 0; sleep 0.12; }
abs() { sendevent $D 3 $1 $2; syn; }           # axis code, value

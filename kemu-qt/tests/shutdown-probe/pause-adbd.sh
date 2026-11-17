#!/system/bin/sh
adbd_pid=$(pidof adbd)
[ -n "$adbd_pid" ] || exit 1
nohup sh -c "sleep 1; kill -STOP $adbd_pid; sleep 30; kill -CONT $adbd_pid" </dev/null >/data/local/tmp/kemu-resume-adbd.log 2>&1 &

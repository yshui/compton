#!/bin/sh
set -xe
if [ -z $DISPLAY ]; then
	exec xvfb-run -s "+extension composite" -a $0 $1 $2 $3
fi

echo "Running test $2"

# TODO keep the log file, and parse it to see if test is successful
($1 --backend dummy --log-level=debug --log-file=$PWD/log --config=$2) &
compton_pid=$!
$3

kill -INT $compton_pid || true
cat log
rm log
wait $compton_pid



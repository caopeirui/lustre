#!/bin/sh
# Utility script to perform minor modifications to the read-write mounted
# snapshot in order to demonstrate the changes w.r.t. the read-only snapshot
OBDDIR="`dirname $0`/.."
. $OBDDIR/demos/config.sh

plog chmod 777 $MNTOBD			# change attributes on an existing file
plog rm $MNTOBD/a			# delete an existing file
echo "echo today >> $MNTOBD/hello"	# modify an existing file
echo today >> $MNTOBD/hello
plog cp /etc/group $MNTOBD		# create a new file
plog ln -s goodbye $MNTOBD/newlink	# create a new symlink

#!/bin/bash
set -e

read -p "Name of SQL file to create (in test/trace_processor): " sqlfile
read -p "Name to trace file (in test/): " tracefile

ROOTDIR=$(dirname $(dirname $(readlink -f $0)))
TEST_PATH=$ROOTDIR/test
TRACE_PROC_PATH=$TEST_PATH/trace_processor

SQL_FILE_NAME=${sqlfile%.*}

echo "Creating $TRACE_PROC_PATH/$sqlfile"
touch $TRACE_PROC_PATH/$sqlfile

TRACE_PATH=$TEST_PATH/$tracefile
TRACE_BASE=$(basename $tracefile)
TRACE_FILE_NAME=${TRACE_BASE%.*}
OUT_FILE="$SQL_FILE_NAME""_$TRACE_FILE_NAME.out"

echo "Creating $TRACE_PROC_PATH/$OUT_FILE"
touch $TRACE_PROC_PATH/$OUT_FILE

RELTRACE=$(realpath -s $TRACE_PATH --relative-to=$TRACE_PROC_PATH --relative-base=$ROOTDIR)
echo "Adding index line to $TRACE_PROC_PATH/index"
echo >> $TRACE_PROC_PATH/index
echo "$RELTRACE $sqlfile $OUT_FILE" >> $TRACE_PROC_PATH/index

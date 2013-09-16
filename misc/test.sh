#!/bin/bash

ANALYZER=`dirname $0`/io-analysis.sh;

CONF=$1;
#OPATH=$2;
#DEVS=$3;

USE_DB=0;
THREADS=1;
MIRROR=0;
MIRROR_PATH=/tmp;

prep_fillrandom() {
	ARGS="--db=$STORE --benchmarks=fillrandom --num=$NUM --use_existing_db=$USE_DB --threads=$THREADS --mirror=$MIRROR --mirror_path=$MIRROR_PATH";
}


READ_FROM=0;
READ_UPTO=-1;
WRITE_FROM=0;
WRITE_UPTO=-1;
BUFFER_SIZE=4194304;
OPEN_FILES=1000;
READ=-1;

prep_rwrandom() {
	ARGS="--db=$STORE --benchmarks=rwrandom --num=$NUM --use_existing_db=$USE_DB --read_percent=$RRATIO --threads=$THREADS --read_key_from=$READ_FROM --read_key_upto=$READ_UPTO --write_key_from=$WRITE_FROM --write_key_upto=$WRITE_UPTO --reads=$READS --write_buffer_size=$BUFFER_SIZE --open_files=$OPEN_FILES --bloom_bits=$BLOOM_BITS --mirror=$MIRROR --mirror_path=$MIRROR_PATH";
}

rwrandom_orig() {
	MIRROR=0;
	BLOOM_BITS=10;
	
	prep_rwrandom;
	$EXEC/db_bench $ARGS &
}

rwrandom_mirror() {
	MIRROR=1;
	BLOOM_BITS=10;
	
	prep_rwrandom;
	$EXEC/db_bench $ARGS &
}

source $CONF; #test, arguments
$TEST;

# sync
wait;

#echo "$EXEC on $STORE according to $CONF -> $OPATH"
#$ANALYZER -d "$DEVS" -o $OPATH -p "

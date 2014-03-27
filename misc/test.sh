#!/bin/bash

ANALYZER=`dirname $0`/io-analysis.sh;

CONF=$1;
TAIL_PARAMS="${*:2}";

USE_DB=0;
MIRROR=0;
MIRROR_PATH=/tmp;
LEVEL_RATIO=4;
FILE_SIZE=8;	# in MiB
COUNTDOWN=-1;
VALUE_SIZE=1024; # default 100
#BUFFER_SIZE=33554432;
#BUFFER_SIZE=67108864;
BUFFER_SIZE=134217728;
THREADS=20;

prep_fillrandom() {
	ARGS="--db=$STORE --benchmarks=fillrandom --num=$NUM --use_existing_db=$USE_DB --threads=$THREADS --mirror=$MIRROR --mirror_path=$MIRROR_PATH --level_ratio=$LEVEL_RATIO --file_size=$FILE_SIZE --histogram=1";
}


READ_FROM=0;
READ_UPTO=-1;
WRITE_FROM=0;
WRITE_UPTO=-1;
OPEN_FILES=1000;
READS=-1;

prep_rwrandom() {
	ARGS="--db=$STORE --benchmarks=rwrandom --num=$NUM --use_existing_db=$USE_DB --value_size=$VALUE_SIZE --read_percent=$RRATIO --threads=$THREADS --read_key_from=$READ_FROM --read_key_upto=$READ_UPTO --write_key_from=$WRITE_FROM --write_key_upto=$WRITE_UPTO --write_buffer_size=$BUFFER_SIZE --open_files=$OPEN_FILES --bloom_bits=$BLOOM_BITS --mirror=$MIRROR --mirror_path=$MIRROR_PATH --level_ratio=$LEVEL_RATIO --file_size=$FILE_SIZE --histogram=1 --countdown=$COUNTDOWN --compression_ratio=1";
	echo "$EXEC/db_bench $ARGS";
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


multi-instance() {
	INSTANCE_NUM=$1;
	echo "Num=$INSTANCE_NUM	Benchmark=$BENCHMARK"

	STORE_ROOT=$STORE;
	MIRROR_ROOT=$MIRROR_PATH;

	for i in `seq 1 $INSTANCE_NUM`; do
		if [ "$BUFFER_SIZES" != "" ]; then
			BUFFER_SIZE="$(echo $BUFFER_SIZES| awk -v ss=$i '{print $ss}')";
			echo $BUFFER_SIZE
		fi
			echo $BUFFER_SIZE

		MIRROR_PATH=$MIRROR_ROOT/${i};
		STORE=$STORE_ROOT/${i};
		echo $MIRROR_PATH $STORE;
		mkdir -p $MIRROR_PATH $STORE;

		$BENCHMARK;
		sleep 1;
	done
}

source $CONF; #test, arguments
cat $CONF;
$TEST $TAIL_PARAMS;

# sync
wait;

#echo "$EXEC on $STORE according to $CONF -> $OPATH"
#$ANALYZER -d "$DEVS" -o $OPATH -p "

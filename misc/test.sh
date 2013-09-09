#!/bin/bash

ANALYZER=`dirname $0`/io-analysis.sh;

CONF=$1;
OPATH=$2;
EXEC=$3;
STORE=$4;
DEVS=$5;

USE_DB=0;
THREADS=1;
source $CONF; #test, benchmark, arguments

prep_fillrandom1 () {
	ARGS="--db=$STORE --benchmarks=$BENCHMARKS --num=$NUM --use_existing_db=$USE_DB --threads=$THREADS";
}

prep_rwrandom1 () {
	ARGS="--db=$STORE --benchmarks=$BENCHMARKS --num=$NUM --use_existing_db=$USE_DB --read_percent=$RRATIO --threads=$THREADS";
}

prep_$TEST;

echo "$EXEC on $STORE according to $CONF -> $OPATH"
$ANALYZER -d "$DEVS" -o $OPATH -p "$EXEC/db_bench $ARGS"


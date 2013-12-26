#!/bin/bash

ANALYZER=/home/siyuan/src/leveldb-m/misc/io-analysis.sh;
TESTER=/home/siyuan/src/leveldb-m/misc/test.sh;
CLEANER=/home/siyuan/src/tp-hive/util/cache-cleanup.sh;

run() {

	local TDIR=$1;
	local RDIR=$2;
	local TAG=$3;
	local CONF=$4;
	local DEVICE=$5;
	local STORE=$6;

	TRACE=$TDIR/$TAG;
	$CLEANER -b;
	sleep 2;
	$ANALYZER -d "$DEVICE" -o "$TRACE" -p "$TESTER $CONF";

	RES=$RDIR/$TAG;
	mkdir -p $RES;

	cd $TRACE && cp $TRACE/*.png $RES;
	cd $TRACE && cp $TRACE/*.out $RES;
	for DEV in $DEVICE; do
		blkparse -i $TRACE/$DEV| tail -100 > $RES/${DEV}.out
	done
	cp $STORE/LOG $RES;

}


D1=sdd;
D2=sdb;
TDIR=/home/siyuan/store/trace/ratio-520;
CDIR=/home/siyuan/store/conf/ratio;
RDIR=/home/siyuan/store/result/ratio-520;

R4STORE=/mnt/ssd520/store/r4;
run $TDIR $RDIR w_1x8_T_r4 "$CDIR/orig_rw_T_r4.conf" "$D1" $R4STORE;
run $TDIR $RDIR w_1x7_T0_r4 "$CDIR/orig_rw_T0_r4.conf" "$D1" $R4STORE;
du -ch /mnt/ssd520/store/;
run $TDIR $RDIR w_2x7_T1_r4 "$CDIR/orig_rw_T1_r4.conf" "$D1" $R4STORE;
du -ch /mnt/ssd520/store/;
run $TDIR $RDIR w_4x7_T2_r4 "$CDIR/orig_rw_T2_r4.conf" "$D1" $R4STORE;
du -ch /mnt/ssd520/store/;
run $TDIR $RDIR w_8x7_T3_r4 "$CDIR/orig_rw_T3_r4.conf" "$D1" $R4STORE;
du -ch /mnt/ssd520/store/;
rm -rf $R4STORE
sleep 8;

R10STORE=/mnt/ssd520/store/r10;
run $TDIR $RDIR w_1x8_T_r10 "$CDIR/orig_rw_T_r10.conf" "$D1" $R10STORE;
#cp -rf $R10STORE /mnt/hdd2/store/r10_base;
run $TDIR $RDIR w_1x7_T0_r10 "$CDIR/orig_rw_T0_r10.conf" "$D1" $R10STORE;
du -ch /mnt/ssd520/store/;
run $TDIR $RDIR w_2x7_T1_r10 "$CDIR/orig_rw_T1_r10.conf" "$D1" $R10STORE;
du -ch /mnt/ssd520/store/;
run $TDIR $RDIR w_4x7_T2_r10 "$CDIR/orig_rw_T2_r10.conf" "$D1" $R10STORE;
du -ch /mnt/ssd520/store/;
run $TDIR $RDIR w_8x7_T3_r10 "$CDIR/orig_rw_T3_r10.conf" "$D1" $R10STORE;
du -ch /mnt/ssd520/store/;


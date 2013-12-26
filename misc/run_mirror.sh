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
	local INUM=$6;

	TRACE=$TDIR/$TAG;
	$CLEANER -b;
	sleep 2;
	$ANALYZER -d "$DEVICE" -o "$TRACE" -p "$TESTER $CONF $INUM";

	RES=$RDIR/$TAG;
	mkdir -p $RES;

	cd $TRACE && cp $TRACE/*.png $RES;
	cd $TRACE && cp $TRACE/*.out $RES;
	for DEV in $DEVICE; do
		blkparse -i $TRACE/$DEV| tail -100 > $RES/${DEV}.out
	done

}

mkbase () {
	local SBASE=$1;
	local SNUM=$2;
	local SDIR=$3;

	mkdir -p $SDIR;
	for i in `seq 1 $SNUM`; do
		cp -rf $SBASE $SDIR/$i;
	done
}

D1=sdd;
D2=sdb;
D3=sdc;
TDIR=/home/siyuan/store/trace/mirror-520;
CDIR=/home/siyuan/store/conf/mirror;
RDIR=/home/siyuan/store/result/mirror-520;

BSTORE=/mnt/hdd2/store/r10_base;
RSTORE=/mnt/ssd520/store/test;
MSTORE=/mnt/hdd1/store/test;

echo "Test Mirror"
#rm -rf $RSTORE $MSTORE;
#mkbase $BSTORE 4 $RSTORE &
#mkbase $BSTORE 4 $MSTORE &
#wait;
#sleep 8;
#run $TDIR $RDIR M_01_T1 "$CDIR/M_01_T1.conf" "$D1 $D2" 4;
#run $TDIR $RDIR M_05_T2 "$CDIR/M_05_T2.conf" "$D1 $D2" 2;
#run $TDIR $RDIR M_1_T3 "$CDIR/M_1_T3.conf" "$D1 $D2" 4;
#run $TDIR $RDIR M_2_T4 "$CDIR/M_2_T4.conf" "$D1 $D2" 4;
#run $TDIR $RDIR M_10_T5 "$CDIR/M_10_T5.conf" "$D1 $D2" 2;

run $TDIR $RDIR M_05_T2_MX "$CDIR/M_05_T2_MX.conf" "$D1 $D2 $D3" 4;
run $TDIR $RDIR M_1_T3_MX "$CDIR/M_1_T3_MX.conf" "$D1 $D2 $D3" 4;
run $TDIR $RDIR M_2_T4_MX "$CDIR/M_2_T4_MX.conf" "$D1 $D2 $D3" 4;
run $TDIR $RDIR M_4_T5_MX "$CDIR/M_4_T5_MX.conf" "$D1 $D2 $D3" 4;
run $TDIR $RDIR M_8_T6_MX "$CDIR/M_8_T6_MX.conf" "$D1 $D2 $D3" 4;
#run $TDIR $RDIR M_10_T5_MX "$CDIR/M_10_T5_MX.conf" "$D1 $D2 $D3" 4;

rm -rf $RSTORE;
mkbase $BSTORE 4 $RSTORE;

echo "Test Original"
#run $TDIR $RDIR O_0_T "$CDIR/O_0_T.conf" "$D1" 4;
#run $TDIR $RDIR O_01_T1 "$CDIR/O_01_T1.conf" "$D1" 4;
run $TDIR $RDIR O_05_T2 "$CDIR/O_05_T2.conf" "$D1" 2;
run $TDIR $RDIR O_1_T3 "$CDIR/O_1_T3.conf" "$D1" 4;
run $TDIR $RDIR O_2_T4 "$CDIR/O_2_T4.conf" "$D1" 4;
run $TDIR $RDIR O_4_T5 "$CDIR/O_4_T5.conf" "$D1" 4;
run $TDIR $RDIR O_8_T6 "$CDIR/O_8_T6.conf" "$D1" 4;
#run $TDIR $RDIR O_10_T5 "$CDIR/O_10_T5.conf" "$D1" 2;
du -ch /mnt/ssd520/store/;



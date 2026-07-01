#!/bin/sh
#
# Legacy test runner for pcompress (t1-t9.tst test suites).
#
# Usage:
#   sh run_test.sh [suite_number]
#
# If no suite number is given, all t*.tst files are run.
# Exit code is non-zero if any FATAL errors were detected.
#

tst=$1

if [ ! -d datafiles ]
then
	mkdir datafiles
else
	rm -f datafiles/.pco*
	rm -f datafiles/*.pz
	rm -f datafiles/*.1
fi
PDIR=$(pwd)

if [ ! -f datafiles/files.lst ]
then
	[ ! -f datafiles/bin.dat ] && (tar cpf - /usr/bin | dd of=datafiles/bin.dat bs=1024 count=5120; cat res/jpg/*.jpg >> datafiles/bin.dat)
	[ ! -f datafiles/share.dat ] && tar cpf - /usr/share | dd of=datafiles/share.dat bs=1024 count=5120
	[ ! -f datafiles/inc.dat ] && (tar cpf - /usr/include | dd of=datafiles/inc.dat bs=1024 count=5120; cat res/xml/*.xml >> datafiles/inc.dat)
	[ ! -f datafiles/combined.dat ] && cat datafiles/bin.dat datafiles/share.dat datafiles/inc.dat >> datafiles/combined.dat
	[ ! -f datafiles/comb_d.dat ] && sh -c "cat datafiles/combined.dat > datafiles/comb_d.dat; cat datafiles/combined.dat >> datafiles/comb_d.dat"

	pdir=$(pwd)
	echo "${pdir}/datafiles/bin.dat" > datafiles/files.lst
	echo "${pdir}/datafiles/share.dat" >> datafiles/files.lst
	echo "${pdir}/datafiles/inc.dat" >> datafiles/files.lst
	echo "${pdir}/datafiles/combined.dat" >> datafiles/files.lst
	echo "${pdir}/datafiles/comb_d.dat" >> datafiles/files.lst
else
	for f in $(cat datafiles/files.lst)
	do
		if [ ! -f "${f}" ]
		then
			echo "Cannot find test data file: ${f}"
			exit 1
		fi
		dir=$(dirname "${f}")
		rm -f "${dir}"/.pco*
		rm -f "${dir}"/*.pz
		rm -f "${dir}"/*.1
	done
fi

failures=0
suites_run=0
suites_passed=0

if [ "x$tst" = "x" ]
then
	for tf in *.tst
	do
		[ ! -f "$tf" ] && continue
		suites_run=$((suites_run + 1))
		echo ""
		echo "========================================="
		echo " Running: $tf"
		echo "========================================="

		cd datafiles
		(. ../${tf}) 2>&1 | tee ${tf}.log
		fails=$(grep -c "^FATAL:" ${tf}.log 2>/dev/null || true)
		fails=${fails:-0}
		failures=$((failures + fails))
		if [ "$fails" -eq 0 ]; then
			suites_passed=$((suites_passed + 1))
		fi
		cd "$PDIR"
	done
else
	tf="t${tst}.tst"
	if [ -f "$tf" ]
	then
		suites_run=1
		cd datafiles
		(. ../${tf})
		if [ $? -ne 0 ]
		then
			echo "FATAL: Test ${tf} failed"
			failures=$((failures + 1))
		else
			suites_passed=1
		fi
		cd "$PDIR"
	else
		echo "No such test $tst"
		exit 1
	fi
fi

echo ""
echo "========================================="
echo " Legacy Test Summary"
echo "========================================="
echo " Suites run:    $suites_run"
echo " Suites passed: $suites_passed"
echo " Total FATAL:   $failures"
echo "========================================="

if [ $failures -gt 0 ]
then
	echo "$failures tests FAILED!"
	exit 1
else
	echo "All tests PASSED"
fi

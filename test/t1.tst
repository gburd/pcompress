#
# Simple compress and decompress
#
echo "#################################################"
echo "# Simple compress and decompress"
echo "#################################################"

_algo_ok() {
	_tmp="${TMPDIR:-/tmp}/_pcomp_probe_$$"
	printf 'probe' > "$_tmp"
	../../pcompress -c "$1" -l 1 -s 64k "$_tmp" 2>/dev/null
	_rv=$?
	rm -f "$_tmp" "$_tmp.pz"
	return $_rv
}

for algo in lzfx lz4 zlib bzip2 lzma lzmaMt libbsc ppmd zstd adapt adapt2
do
	_algo_ok $algo || continue

	for level in 1 3 9 14
	do
		for tf in `cat files.lst`
		do
			for seg in 1m 100m
			do
				[ $level -lt 14 -a "$seg" = "100m" ] && continue

				cmd="../../pcompress -c ${algo} -l ${level} -s ${seg} ${tf}"
				echo "Running $cmd"
				eval $cmd
				if [ $? -ne 0 ]
				then
					echo "FATAL: Compression failed."
					rm -f ${tf}.pz
					continue
				fi
				cmd="../../pcompress -d ${tf}.pz ${tf}.1"
				echo "Running $cmd"
				eval $cmd
				if [ $? -ne 0 ]
				then
					echo "FATAL: Decompression failed."
					rm -f ${tf}.pz ${tf}.1
					continue
				fi
				diff ${tf} ${tf}.1 > /dev/null
				if [ $? -ne 0 ]
				then
					echo "FATAL: Decompression was not correct"
				fi
				rm -f ${tf}.pz ${tf}.1
			done
		done
	done
done

echo "#################################################"
echo ""


# Helper to create files with unique contents

test_create_unique_files_base__=$(date -u)
test_create_unique_files_counter__=0

# Create multiple files with unique contents. Takes the number of
# directories, the number of files in each directory, and the base
# directory.
#
# test_create_unique_files 2 3 . -- Creates 2 directories with 3 files
#				    each in the specified directory, all
#				    with unique contents.

test_create_unique_files() {
	test "$#" -ne 3 && BUG "3 param"

	local dirs=$1
	local files=$2
	local basedir=$3

	rm -rf $basedir >/dev/null

	for i in $(test_seq $dirs)
	do
		local dir=$basedir/dir$i

		mkdir -p "$dir" > /dev/null
		for j in $(test_seq $files)
		do
			test_create_unique_files_counter__=$((test_create_unique_files_counter__ + 1))
			echo "$test_create_unique_files_base__.$test_create_unique_files_counter__"  >"$dir/file$j.txt"
		done
	done
}

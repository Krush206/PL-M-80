#!/usr/bin/env bash

# usage: <string> -> <number>
# cli arguments: <string>
# cli output: <number>

function print_usage {
    echo "Usage: str_int_test.sh <string>"
    echo "<string> represents the string to convert to integer."
}

if [ $# -ne 1 ]
then
    print_usage
    exit 1
fi

# The first two bytes of the image contain the little endian representation
# of the result.

asm_file=../tools.asm
bin_file=str_int.bin

# Get the CLI arguments
string="$1"

# Place CLI arguments into the code
tmp_file=$(mktemp)
sed str_int_test.asm -e "s/^str:.*$/str:\tdb\t\"$string\", 0/" > "$tmp_file"

# Assemble
z88dk-z80asm -b "$tmp_file" ../tools.asm -o="$bin_file"

# Execute
z88dk-ticks "$bin_file" -output "rom.bin" > /dev/null

# Print the result
od -An -t u2 -N 2 -j 0 "rom.bin"

# Clean
rm -f "$tmp_file" "rom.bin" "$bin_file"

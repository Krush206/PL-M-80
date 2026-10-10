#!/usr/bin/env bash

file_name=$1
fun_name=$2

include=false
while IFS="" read -r l || [ -n "$l" ]
do
    if [[ $l =~ end[[:space:]]procedure ]]
    then
        if [[ $include = true ]]
        then
            printf '%s\n' "$l"
        fi
        include=false
    elif [[ $l =~ $fun_name: ]]
    then
        printf '%s\n' "$l"
        include=true
    elif [[ $include = true ]]
    then
        printf '%s\n' "$l"
    fi
done < "$file_name"

#	; end procedure fib8

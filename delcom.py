#!/usr/bin/env python3

import sys

def delcom(str):
    output = ""
    i = 0;
    have_first = False
    is_com = False
    while i < len(str):
        c = str[i]
        if is_com:
            if have_first:
                if c == '/':
                    is_com = False
            have_first = (c == '*')
        else:
            if have_first:
                if c != '*':
                    output += '/'
                    if c != '/':
                        output += c
                is_com = (c == '*')
            elif c != '/':
                output += c
            have_first = (c == '/')
        i = i + 1
    return output

def main():
    if len(sys.argv) != 3:
        print(f"usage {sys.argv[0]} <in_file> <out_file>")
        exit(0)
    in_file = sys.argv[1]
    out_file = sys.argv[2]
    with open(in_file, 'r') as f_in:
        input = f_in.read()
    output = delcom(input)
    with open(out_file, 'w') as f_out:
        f_out.write(output)

if __name__ == "__main__":
    main()

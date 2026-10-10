#!/usr/bin/env python

import sys
import re

def repl(m, k, v):
    if m.group(1):
        s = m.group(1)
        t = re.sub(fr"(^|[^a-zA-Z0-9$]){k}($|[^a-zA-Z0-9$])", rf"\1\v{v}\2", s)
        return t.replace('\v', '')
    else:
        return m.group(0)

def do_lit(data):
    lits = {}
    output = ""
    for line in data.splitlines():
        m = re.search("^declare\s+(\S+)\s+literally\s+'(\S+)'\s*;$", line.strip())
        if m:
            key = m.group(1).replace('$', '\$')
            lits[key] = m.group(2)
            #output += line
        else:
            l = line
            for lit in lits:
                f = lambda m0 : repl(m0, lit, lits[lit])
                l = re.sub(r"'[^']*'|([^']+)", f, l) # ignore quotes strings
            output += l
        output += "\n"
    return output

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file>")
        exit(0)
    in_filename = sys.argv[1]
    file = open(in_filename, 'r')
    file_data = file.read()
    print(do_lit(file_data))

if __name__ == "__main__":
    main()

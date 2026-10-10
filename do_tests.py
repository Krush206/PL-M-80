#!/usr/bin/env python

import subprocess

hello_tests = ['']
fib_tests = [0, 1, 2, 5, 10, 15, 20]
palin_tests = ['', 'e', 'rr', 'er', 're', 'rer', '1221', '1231', '12321', '12321a']
sort_tests = [ '', 'a', 'ab', 'ba', 'aa', 'aab', 'baa', 'bab', 'aba', 'abc', 'cba']
heap_tests = [ [], [0], [1,1], [1,2], [2,1], [0,0], [1,2,3], [3,2,1], [3,1,2],
    [1,2,3,4], [4,3,2,1], [4,2,1,3], [1,4,2,3], [1,2,3,4,5], [5,3,2,2,1] ]
primes_tests = ['']
labels_tests = ['']

programs = {'hello': hello_tests,
            'fib': fib_tests,
            'palin': palin_tests,
            'sort': sort_tests,
            'heap': heap_tests,
            'primes': primes_tests,
            'labels': labels_tests}

for (program, tests) in programs.items():
    print(f'program: {program}')
    directory = f"tests/{program}"

    # build
    print(f'building: {program}')
    subprocess.call(["make", "-C", directory])

    # run tests
    print(f'running tests for program {program}:')
    prog1 = f'{directory}/{program}.com'
    prog2 = f'{directory}/{program}2.com'
    for test in tests:
        if type(test) == list:
            params = ' '.join(map(str, test))
        else:
            params = str(test)
        print(f'\ttesting: "{params}"', end="")
        cmd1 = ['zxcc', prog1] + [f'-{params}']
        out1 = subprocess.check_output(cmd1, text=True).strip()
        cmd2 = ['zxcc', prog2] + [f'-{params}']
        out2 = subprocess.check_output(cmd2, text=True).strip()
        if out1 == out2:
            if '\n' in out1:
                print()
            else:
                print(f', result={out1}')
        else:
            print(f', ***ERROR***')
            print(f'\t{out1} != {out2}')
            break
    print(f'OK')

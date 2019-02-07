#!/bin/bash

try_direct() {
  expected="$1"
  input="$2"

  ./xcc "$input" > tmp || exit 1
  chmod +x tmp
  ./tmp
  actual="$?"

  if [ "$actual" = "$expected" ]; then
    echo "$input => $actual"
  else
    echo "ERROR: $input => $expected expected, but got $actual"
    exit 1
  fi
}

try() {
  try_direct "$1" "int main(){ $2 }"
}

try_output_direct() {
  expected="$1"
  input="$2"

  ./xcc "$input" > tmp || exit 1
  chmod +x tmp
  actual=`./tmp` || exit 1

  if [ "$actual" = "$expected" ]; then
    echo "$input => $actual"
  else
    echo "ERROR: $input => $expected expected, but got $actual"
    exit 1
  fi
}

try_output() {
  try_output_direct "$1" "int main(){ $2 0; }"
}

try 0 '0;'
try 42 '42;'
try 21 '5+20-4;'
try 41 " 12 + 34 - 5 ;"
try 47 "5+6*7;"
try 15 "5*(9-6);"
try 4 "(3+5)/2;"
try 14 "int a; int b; a = 3; b = 5 * 6 - 8; a + b / 2;"
try 14 "int foo; int bar; foo = 3; bar = 5 * 6 - 8; foo + bar / 2;"
try 1 "int a; int b; int c; a = b = (c = 1) + 2; a == b;"
try 1 "123 != 456;"
try_direct 23 "int foo(){ 123; } int main(){ foo() - 100; }"
try_direct 9 "int sqsub(int x, int y){ int xx; int yy; xx = x * x; yy = y * y; xx - yy; } int main(){ sqsub(5, 4); }"
try 2 "if (1) 2;"
try 3 "if (1 == 0) 2; else 3;"
try 3 "int a; int b; a = b = 0; if (1) { a = 1; b = 2; } a + b;"
try 55 "int i; int acc; i = acc = 0; while (i != 11) { acc = acc + i; i = i + 1; } acc;"
try 55 "int i; int acc; for (i = acc = 0; i != 11; i = i + 1) acc = acc + i; acc;"
try 11 "int *p; int x; x = 10; p = &x; *p = *p + 1; x;"
try 123 "int a[3]; int *p; p = a; p = p + 1; *p = 123; *(a + 1);"
try 11 "int a[2]; *a = 1; a[1] = 10; a[0] + 1[a];"
try_direct 11 "int x; int main(){ x = 1; x + 10; }"
try_output 'hello' '_write(1, "hello\n", 6);'

echo OK

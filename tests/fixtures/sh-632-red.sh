#!/bin/sh
# ITEM-632 RED/GREEN fixture: assignments, functions, if/elif, for/while/until,
# positional parameters, command substitution and a subshell.  The ITEM-628
# shell rejects the first compound command; ITEM-632 must produce exactly the
# output compared by the production QEMU proof.
greet() {
    echo "hello $1"
}

x=outer
for name in "$@"; do
    greet "$name"
done

if [ "$x" = outer ]; then
    echo "if-ok"
elif [ "$x" = other ]; then
    echo "if-wrong"
else
    echo "if-bad"
fi

for i in a b c; do
    if [ "$i" = b ]; then
        continue
    fi
    echo "i=$i"
done

while false; do
    echo "while-bad"
done

until true; do
    echo "until-bad"
done

f() {
    echo "f:$1"
    return 3
}
f value
echo "ret=$?"

echo "args=$#"
echo "first=$1"
echo "sub=$(echo inner)"
(cd /tmp && pwd)
echo "outer=$x"
( x=inner; echo "subx=$x" )
echo "outer2=$x"
trap 'echo trap-ok' EXIT
echo end

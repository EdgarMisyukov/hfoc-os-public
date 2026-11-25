echo quoted > /tmp/sh-out
echo 'a b' >> /tmp/sh-out
false || echo or-ok >> /tmp/sh-out
true && echo and-ok >> /tmp/sh-out
echo PIPE | cat > /tmp/sh-pipe
cat /missing > /tmp/sh-err 2>&1
echo "status=$?" >> /tmp/sh-out
cat /missing > /tmp/sh-out-only 2>/tmp/sh-err-sep
echo "sep=$?" > /tmp/sh-sep-status
nosuchcommand > /tmp/sh-out-only 2>/tmp/sh-nf-err
cat /tmp/sh-nf-err >> /tmp/sh-err
nosuchcommand > /tmp/sh-nf-err 2>&1
echo "nf=$?" >> /tmp/sh-sep-status

#!/bin/sh

SRC=profile/firefox-10.0esr.tar
DST=profile/firefox-17.0esr.tar
SRCSIZE=39526400
DSTSIZE=49192960
SRCURL=https://ftp.mozilla.org/pub/firefox/releases/10.0esr/linux-x86_64/en-US/firefox-10.0esr.tar.bz2
DSTURL=https://ftp.mozilla.org/pub/firefox/releases/17.0esr/linux-x86_64/en-US/firefox-17.0esr.tar.bz2

if [ ! -f $SRC ] || [ $(wc -c <$SRC) -lt $SRCSIZE ]; then
  wget $SRCURL -O- | bzcat > $SRC || exit $?
fi
if [ ! -f $DST ] || [ $(wc -c <$DST) -lt $DSTSIZE ]; then
  wget $DSTURL -O- | bzcat > $DST || exit $?
fi

echo y > profile/choice

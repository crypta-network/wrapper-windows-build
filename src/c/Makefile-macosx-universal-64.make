# Copyright (c) 1999, 2025 Tanuki Software, Ltd.
# http://www.tanukisoftware.com
# All rights reserved.
#
# This software is the proprietary information of Tanuki Software.
# You shall use it only in accordance with the terms of the
# license agreement you entered into with Tanuki Software.
# http://wrapper.tanukisoftware.com/doc/english/licenseOverview.html

# Newer SDKs work differently so we don't want to specify where it is.
ifdef DEV_AUTOSDK
	ISYSROOT=
	DEV_NOPPC=true
	DEFS=-I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/darwin
else
	ifdef DEV_NOPPC
		UNIVERSAL_SDK_HOME=/Library/Developer/CommandLineTools/SDKs/MacOSX10.15.sdk
	else
		UNIVERSAL_SDK_HOME=/Developer/SDKs/MacOSX10.5.sdk
	endif
	ISYSROOT=-isysroot $(UNIVERSAL_SDK_HOME)
	DEFS=-I$(UNIVERSAL_SDK_HOME)/System/Library/Frameworks/JavaVM.framework/Headers
endif

# If you define DEV_NOPPC on your development machine then ppc64 will not be built.  This is needed for newer XCode versions. This should never be done on release build machines.
ifdef DEV_NOPPC
	ARCHPPC=
else
	ARCHPPC=-arch ppc64
endif
COMPILE = gcc -O3 -m64 -Wall -DUSE_NANOSLEEP -DMACOSX -D_FORTIFY_SOURCE=2 -DJSW64 $(ARCHPPC) -arch x86_64 $(ISYSROOT) -mmacosx-version-min=10.4 -DUNICODE -D_UNICODE

wrapper_SOURCE = wrapper.c wrapperinfo.c wrappereventloop.c wrapper_jvm_launch.c wrapper_unix.c property.c logger.c logger_file.c wrapper_file.c wrapper_i18n.c wrapper_hashmap.c wrapper_ulimit.c wrapper_encoding.c wrapper_jvminfo.c wrapper_secure_file.c wrapper_sysinfo.c wrapper_cipher.c wrapper_cipher_base.c

libwrapper_so_OBJECTS = wrapper_i18n.o wrapperjni_unix.o wrapperinfo.o wrapperjni.o loggerjni.o

BIN = ../../bin
LIB = ../../lib

all: init wrapper libwrapper.jnilib

clean:
	rm -f *.o

cleanall: clean
	rm -rf *~ .deps
	rm -f $(BIN)/wrapper $(LIB)/libwrapper.jnilib

init:
	if test ! -d .deps; then mkdir .deps; fi

wrapper: $(wrapper_SOURCE)
	$(COMPILE) $(wrapper_SOURCE) -liconv -pthread -o $(BIN)/wrapper

libwrapper.jnilib: $(libwrapper_so_OBJECTS)
	$(COMPILE) -bundle -liconv -pthread -o $(LIB)/libwrapper.jnilib $(libwrapper_so_OBJECTS)

%.o: %.c
	$(COMPILE) -c $(DEFS) $<


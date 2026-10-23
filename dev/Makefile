PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

PAYLOAD_VERSION ?= v1.0.0
ELF := PlayTime-$(PAYLOAD_VERSION).elf

SOURCES := main.c games.c server.c ui.c sqlite/sqlite3.c
HEADERS := config.h games.h server.h ui.h sqlite/sqlite3.h

CPPFLAGS := -DPAYLOAD_VERSION=\"$(PAYLOAD_VERSION)\" \
            -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
            -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_TEMP_STORE=2
CFLAGS := -Wall -Werror -Os
LDLIBS := -lkernel_sys -lSceUserService -lSceSystemService

.PHONY: all clean test debug

all: $(ELF)

$(ELF): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f PlayTime-*.elf

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

debug: $(ELF)
	gdb-multiarch \
	-ex "set architecture i386:x86-64" \
	-ex "target extended-remote $(PS5_HOST):2159" \
	-ex "file $(ELF)" \
	-ex "remote put $(ELF) /data/$(ELF)" \
	-ex "set remote exec-file /data/$(ELF)" \
	-ex "start"

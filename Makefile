CC      ?= cc
R_HOME  := $(shell R RHOME)
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g
CPPFLAGS += -DR_INTERFACE_PTRS -DCSTACK_DEFNS -DROPE_R_HOME='"$(R_HOME)"' \
            $(shell R CMD config --cppflags)
LDFLAGS += $(shell R CMD config --ldflags) -Wl,-rpath,$(R_HOME)/lib
LDLIBS  += -lm

# The line editor: a vendored copy of isocline (MIT, see $(ISOCLINE)/LICENSE)
# with a few marked "rope:" additions. Its sources form one translation unit.
ISOCLINE ?= src/isocline
ISOCLINE_CPPFLAGS := -I$(ISOCLINE)/include -DIC_MAX_HISTORY=10000

SRC := src/rope.c
OBJ := rope.o graphics.o isocline.o

rope: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

rope.o: $(SRC) $(ISOCLINE)/include/isocline.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(ISOCLINE_CPPFLAGS) -c -o $@ $(SRC)

# The graphics device, with its own rasteriser; text comes from the vendored
# stb_truetype.h (public domain).
graphics.o: src/graphics.c src/stb_truetype.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ src/graphics.c

isocline.o: $(ISOCLINE)/src/*.c $(ISOCLINE)/src/*.h $(ISOCLINE)/include/isocline.h
	$(CC) -std=c11 -O2 -g -w $(ISOCLINE_CPPFLAGS) -c -o $@ $(ISOCLINE)/src/isocline.c

debug: CFLAGS := -std=c11 -Wall -Wextra -O0 -g -fsanitize=address,undefined
debug: clean rope

check: rope
	sh tests/smoke.sh
	python3 tests/pty_test.py
	python3 tests/graphics_test.py

clean:
	rm -f rope *.o

.PHONY: debug check clean

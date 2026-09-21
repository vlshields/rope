CC      ?= cc
R_HOME  := $(shell R RHOME)
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g
CPPFLAGS += -DR_INTERFACE_PTRS -DCSTACK_DEFNS -DROPE_R_HOME='"$(R_HOME)"' \
            $(shell R CMD config --cppflags)
LDFLAGS += $(shell R CMD config --ldflags) -Wl,-rpath,$(R_HOME)/lib
LDLIBS  += -lreadline

SRC := src/rope.c

rope: $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

debug: CFLAGS := -std=c11 -Wall -Wextra -O0 -g -fsanitize=address,undefined
debug: rope

check: rope
	sh tests/smoke.sh
	python3 tests/pty_test.py

clean:
	rm -f rope *.o

.PHONY: debug check clean

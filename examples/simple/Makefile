CHARM_HOME ?= $(HOME)/charm-paratreet
BASE_PATH=$(shell realpath "$(shell pwd)/../..")
PARATREET_PATH = $(BASE_PATH)/src
STRUCTURE_PATH = $(BASE_PATH)/utility/structures
OPTS = -g -I$(STRUCTURE_PATH) -I$(PARATREET_PATH) -DCOUNT_INTERACTIONS=0 -DDEBUG=0 $(MAKE_OPTS)
CHARMC = $(CHARM_HOME)/bin/charmc $(OPTS)
LD_LIBS = -L$(PARATREET_PATH) -lparatreet

BINARY = Main
OBJS = Main.o

all: $(BINARY) $(BINARY)_proj

$(BINARY).decl.h: $(BINARY).ci
	$(CHARMC) $<

$(BINARY): $(BINARY).decl.h $(OBJS)
	$(CHARMC) -language charm++ -o $(BINARY) $(OBJS) $(LD_LIBS)

$(BINARY)_proj: $(BINARY).decl.h $(OBJS)
	$(CHARMC) -language charm++ -tracemode projections -o $(BINARY)_proj $(OBJS) $(LD_LIBS)

Main.o: Main.C
	$(CHARMC) -c $<

test: all
	./charmrun ./$(BINARY) -f $(BASE_PATH)/inputgen/100k.tipsy +p3 ++ppn 3 +pemap 1-3 +commap 0 ++local

run: all
	./$(BINARY) -u 2  -f $(BASE_PATH)/inputgen/100k.tipsy

proj: all
	./charmrun ./$(BINARY)_proj -f $(BASE_PATH)/inputgen/100k.tipsy +p20 +setcpuaffinity

clean:
	rm -f *.decl.h *.def.h conv-host *.o $(BINARY) charmrun

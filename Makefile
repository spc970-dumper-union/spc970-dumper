EE_BIN = ps2_mecha_tool.elf
EE_BIN_STRIPPED = ps2_mecha_tool_stripped.elf
EE_OBJS = main.o mecha.o verify.o ident_db.o logger.o

EE_INCS := -I. -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include -I$(PS2SDK)/ports/include
EE_LDFLAGS := -L$(PS2SDK)/ee/lib -L$(PS2SDK)/ports/lib
EE_LIBS = -ldebug -lcdvd -lpadx -lps2_drivers -lpatches -lkernel

all: $(EE_BIN) $(EE_BIN_STRIPPED)

$(EE_BIN_STRIPPED): $(EE_BIN)
	$(EE_STRIP) -s -o $(EE_BIN_STRIPPED) $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_BIN_STRIPPED) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

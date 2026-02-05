# ──────────────────────────────────────────────────────────────
# OpusEdit – Terminal Text Editor
# ──────────────────────────────────────────────────────────────

CC        := cc
CFLAGS    := -std=c11 -Wall -Wextra -Wpedantic -Wstrict-prototypes \
             -Wshadow -Wconversion -Wno-sign-conversion -D_POSIX_C_SOURCE=200809L
INCLUDES  := -Iinclude
LDFLAGS   :=

SRCDIR    := src
INCDIR    := include
BINDIR    := bin
OBJDIR    := obj

SRCS      := $(wildcard $(SRCDIR)/*.c)
OBJS      := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
TARGET    := $(BINDIR)/opusedit

# ── Platform detection ──────────────────────────────────────
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
endif
ifeq ($(UNAME_S),Linux)
    CFLAGS += -D_GNU_SOURCE
endif

# ── Default target ──────────────────────────────────────────
.PHONY: all clean debug release

all: CFLAGS += -O2
all: $(TARGET)

debug: CFLAGS += -g -O0 -DDEBUG -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)

release: CFLAGS += -O3 -DNDEBUG
release: $(TARGET)

# ── Build rules ─────────────────────────────────────────────
$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	@echo "  LINK  $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@echo "  CC    $<"

$(BINDIR) $(OBJDIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "  CLEAN"

# ── Header dependencies ────────────────────────────────────
$(OBJDIR)/main.o:      $(INCDIR)/editor.h $(INCDIR)/terminal.h $(INCDIR)/input.h $(INCDIR)/output.h $(INCDIR)/file_io.h
$(OBJDIR)/editor.o:    $(INCDIR)/editor.h $(INCDIR)/terminal.h $(INCDIR)/input.h $(INCDIR)/output.h
$(OBJDIR)/terminal.o:  $(INCDIR)/editor.h $(INCDIR)/terminal.h $(INCDIR)/output.h
$(OBJDIR)/input.o:     $(INCDIR)/editor.h $(INCDIR)/input.h $(INCDIR)/buffer.h $(INCDIR)/output.h $(INCDIR)/find.h $(INCDIR)/file_io.h
$(OBJDIR)/buffer.o:    $(INCDIR)/editor.h $(INCDIR)/buffer.h $(INCDIR)/output.h
$(OBJDIR)/output.o:    $(INCDIR)/editor.h $(INCDIR)/output.h $(INCDIR)/buffer.h
$(OBJDIR)/file_io.o:   $(INCDIR)/editor.h $(INCDIR)/file_io.h $(INCDIR)/buffer.h $(INCDIR)/output.h
$(OBJDIR)/find.o:      $(INCDIR)/editor.h $(INCDIR)/find.h $(INCDIR)/input.h $(INCDIR)/output.h

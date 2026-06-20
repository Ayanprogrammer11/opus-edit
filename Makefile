# ──────────────────────────────────────────────────────────────
# OpusEdit – Terminal Text Editor
# ──────────────────────────────────────────────────────────────

CC        := cc
CFLAGS    := -std=c11 -Wall -Wextra -Wpedantic -Wstrict-prototypes \
             -Wshadow -Wconversion -Wno-sign-conversion -D_POSIX_C_SOURCE=200809L
INCLUDES  := -Iinclude
LDFLAGS   := -lz

SRCDIR    := src
INCDIR    := include
TESTDIR   := tests
BINDIR    := bin
OBJDIR    := obj

SRCS      := $(wildcard $(SRCDIR)/*.c)
OBJS      := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
TARGET    := $(BINDIR)/opusedit
UNIT_TARGET := $(BINDIR)/unit_core_tests
UNIT_SRCS := $(filter-out $(SRCDIR)/main.c,$(SRCS)) $(TESTDIR)/unit_core.c

# ── Platform detection ──────────────────────────────────────
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
endif
ifeq ($(UNAME_S),Linux)
    CFLAGS += -D_GNU_SOURCE
endif

# ── Default target ──────────────────────────────────────────
.PHONY: all clean debug release test unit-test workflow-test

all: CFLAGS += -O2
all: $(TARGET)

debug: CFLAGS += -g -O0 -DDEBUG -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)

release: CFLAGS += -O3 -DNDEBUG
release: $(TARGET)

test: unit-test workflow-test

unit-test: CFLAGS += -g -O0 -DDEBUG -fsanitize=address,undefined
unit-test: LDFLAGS += -fsanitize=address,undefined
unit-test: $(UNIT_TARGET)
	./$(UNIT_TARGET)

workflow-test: all
	OPUSEDIT_BIN="$(abspath $(TARGET))" python3 $(TESTDIR)/workflow_tests.py

# ── Build rules ─────────────────────────────────────────────
$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	@echo "  LINK  $@"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@echo "  CC    $<"

$(UNIT_TARGET): $(UNIT_SRCS) | $(BINDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)
	@echo "  TEST-LINK $@"

$(BINDIR) $(OBJDIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "  CLEAN"

# ── Header dependencies ────────────────────────────────────
$(OBJDIR)/main.o:      $(INCDIR)/editor.h $(INCDIR)/terminal.h $(INCDIR)/input.h $(INCDIR)/output.h $(INCDIR)/file_io.h
$(OBJDIR)/editor.o:    $(INCDIR)/editor.h $(INCDIR)/terminal.h $(INCDIR)/input.h $(INCDIR)/output.h $(INCDIR)/undo.h $(INCDIR)/git.h
$(OBJDIR)/terminal.o:  $(INCDIR)/editor.h $(INCDIR)/terminal.h $(INCDIR)/output.h
$(OBJDIR)/input.o:     $(INCDIR)/editor.h $(INCDIR)/input.h $(INCDIR)/buffer.h $(INCDIR)/output.h $(INCDIR)/find.h $(INCDIR)/file_io.h $(INCDIR)/git.h $(INCDIR)/undo.h
$(OBJDIR)/buffer.o:    $(INCDIR)/editor.h $(INCDIR)/buffer.h $(INCDIR)/output.h $(INCDIR)/undo.h $(INCDIR)/git.h
$(OBJDIR)/output.o:    $(INCDIR)/editor.h $(INCDIR)/output.h $(INCDIR)/buffer.h $(INCDIR)/git.h
$(OBJDIR)/file_io.o:   $(INCDIR)/editor.h $(INCDIR)/file_io.h $(INCDIR)/buffer.h $(INCDIR)/output.h $(INCDIR)/git.h
$(OBJDIR)/find.o:      $(INCDIR)/editor.h $(INCDIR)/find.h $(INCDIR)/input.h $(INCDIR)/output.h $(INCDIR)/buffer.h $(INCDIR)/undo.h
$(OBJDIR)/undo.o:      $(INCDIR)/editor.h $(INCDIR)/undo.h $(INCDIR)/buffer.h $(INCDIR)/git.h $(INCDIR)/output.h

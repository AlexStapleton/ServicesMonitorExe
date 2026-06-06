# Makefile — ServicesExeMonitor (multi-file build)
# Requires: MinGW-w64 g++ with C++20 support
#
# Temp dir: some MSYS2 setups hand the native g++ a TMP/TEMP value in MSYS
# (/c/...) form that it can't use, so it falls back to the read-only C:\Windows
# and the build dies with "Cannot create temporary file". Give it a Windows-form
# path to the build's own obj/ folder instead, computed portably via cygpath so
# it works on any machine (including CI) — unlike a hardcoded absolute path.
$(shell mkdir -p obj)
WINTMP := $(shell cygpath -w "$(CURDIR)/obj" 2>/dev/null)
export TMP    := $(WINTMP)
export TEMP   := $(WINTMP)
export TMPDIR := $(WINTMP)

CXX      = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DIDI_APPICON=101 -ffunction-sections -fdata-sections
LDFLAGS  = -mwindows -municode -static -s -Wl,--gc-sections
LIBS     = -lcomctl32 -ladvapi32 -lole32 -lshlwapi -lcomdlg32 -lshell32 -ldwmapi -luxtheme

SRCDIR   = src
OBJDIR   = obj
TARGET   = ServicesExeMonitor.exe

SRCS = \
	$(SRCDIR)/util.cpp         \
	$(SRCDIR)/app.cpp          \
	$(SRCDIR)/darkmode.cpp     \
	$(SRCDIR)/tray.cpp         \
	$(SRCDIR)/theme.cpp        \
	$(SRCDIR)/config.cpp       \
	$(SRCDIR)/profiles.cpp     \
	$(SRCDIR)/monitor.cpp      \
	$(SRCDIR)/action.cpp       \
	$(SRCDIR)/ui_listview.cpp  \
	$(SRCDIR)/ui_layout.cpp    \
	$(SRCDIR)/ui_dialogs.cpp   \
	$(SRCDIR)/main.cpp

OBJS = $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS))

# Resource file (icon + version info)
RCFILE = app.rc
RESOBJ = $(OBJDIR)/app.res.o

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Link
$(TARGET): $(OBJS) $(if $(wildcard $(RCFILE)),$(RESOBJ))
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

# Compile .cpp -> .o
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(SRCDIR)/app.h $(SRCDIR)/util.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile .rc -> .o
$(OBJDIR)/app.res.o: $(RCFILE) app.ico app.manifest
	windres $< -o $@

# Syntax check only (useful when linker is broken)
syntax: $(SRCS)
	$(CXX) $(CXXFLAGS) -fsyntax-only $(SRCS)

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean syntax

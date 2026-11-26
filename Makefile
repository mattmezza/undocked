CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/undocked
VERSION ?= 0.1.2
BUILD ?= build
PACKAGES = x11 xrandr freetype2 harfbuzz

CPPFLAGS += -D_GNU_SOURCE -DCUI_HAS_TEXT=1 -Ivendor/cui/include -Ivendor/cui/third_party/clay \
            $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
CFLAGS += -std=c11 -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
ifeq ($(STATIC),1)
LDFLAGS += -static
LDLIBS += -Wl,--start-group $(shell $(PKG_CONFIG) --static --libs $(PACKAGES)) -lstdc++ -pthread -lm -Wl,--end-group
else
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm
endif

APP = $(patsubst %.c,$(BUILD)/%.o,$(wildcard src/*.c))
CUI = $(addprefix $(BUILD)/vendor/cui/,src/core/cui.o src/core/cui_background.o \
       src/effects/cui_effects.o src/text/cui_text.o backends/software/cui_software.o)
OBJECTS = $(APP) $(CUI) $(BUILD)/assets/fonts.o
WARNINGS = -Wall -Wextra -Wpedantic

.PHONY: all check clean install release
ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif
all: $(BUILD)/undocked

$(BUILD)/undocked: $(OBJECTS) Makefile
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD)/src/main.o: CPPFLAGS += -DUNDOCKED_VERSION='"$(VERSION)"'
$(BUILD)/src/%.o: src/%.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

$(BUILD)/vendor/%.o: vendor/%.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/assets/fonts.o: assets/fonts.S assets/Geist-Regular.ttf assets/Geist-Medium.ttf assets/GeistMono-Regular.ttf assets/icons.alpha
	@mkdir -p $(@D)
	$(CC) -c $< -o $@

-include $(OBJECTS:.o=.d)

check: all
	xvfb-run -a -s '-screen 0 1440x900x24' python3 tests/x11.py "$(BUILD)/undocked"

install: all
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(DATADIR)"
	install -m 755 "$(BUILD)/undocked" "$(DESTDIR)$(BINDIR)/undocked"
	install -m 644 config.ini LICENSE assets/*-LICENSE "$(DESTDIR)$(DATADIR)/"
	install -m 644 vendor/cui/LICENSE "$(DESTDIR)$(DATADIR)/CUI-LICENSE"
	install -m 644 vendor/cui/third_party/clay/LICENSE "$(DESTDIR)$(DATADIR)/CLAY-LICENSE"

clean:
	rm -rf "$(BUILD)"

release: export name := $(name)
release:
	sh tools/release.sh

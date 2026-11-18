TARGET = AppBundleExe

DEBUG   ?= 0
ARCH    ?= x86_64
SDK     ?= macosx
OSXVER  ?= 10.13

SYSROOT  := $(shell xcrun --sdk $(SDK) --show-sdk-path)
ifeq ($(SYSROOT),)
$(error Could not find SDK "$(SDK)")
endif
CLANG    := clang
CC       := $(CLANG) -isysroot $(SYSROOT) -arch $(ARCH) -mmacosx-version-min=$(OSXVER)

CFLAGS  = -O1 -Wall -g -I include/ -I src/
LDFLAGS = 

FRAMEWORKS = -framework CoreFoundation -framework Foundation

# StartWithMod's rules and the settings.ini reader, the library's own files (source/start_mod.c with the command
# line's in launch_args.c, source/settings_ini.c)
SOURCES = launcher.m ../source/start_mod.c ../source/launch_args.c ../source/settings_ini.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(FRAMEWORKS) $(DEFINES) $(LDFLAGS) -o $@ $(SOURCES)

clean:
	rm -f -- $(TARGET)

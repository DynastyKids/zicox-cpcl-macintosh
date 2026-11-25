CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-deprecated-declarations -Wno-unused-parameter
ARCHS   ?= -arch arm64 -arch x86_64
MINOS   ?= -mmacosx-version-min=11.0

APP = build/ZicoxBluetooth.app
BIN = build/rastertozicox $(APP)/Contents/MacOS/zicox-btrelay build/zicoxctl

all: $(BIN) ppd/Zicox_CC4.ppd

build:
	mkdir -p build

build/rastertozicox: src/rastertozicox.c | build
	$(CC) $(CFLAGS) $(ARCHS) $(MINOS) -o $@ $< -lcups -lz

$(APP)/Contents/MacOS/zicox-btrelay: src/zicox-btrelay.m src/zcio.h src/Info.plist | build
	mkdir -p $(APP)/Contents/MacOS
	cp src/Info.plist $(APP)/Contents/Info.plist
	$(CC) $(CFLAGS) -fobjc-arc $(ARCHS) $(MINOS) -o $@ $< -framework Foundation -framework IOBluetooth \
	    -sectcreate __TEXT __info_plist src/Info.plist
	codesign -s - -f -i com.zicox.btrelay $(APP)

build/zicoxctl: src/zicoxctl.c src/zcio.h | build
	$(CC) $(CFLAGS) $(ARCHS) $(MINOS) -o $@ $< -liconv

ppd/Zicox_CC4.ppd: tools/gen_ppd.py
	mkdir -p ppd
	python3 -I tools/gen_ppd.py > $@

clean:
	rm -rf build

.PHONY: all clean

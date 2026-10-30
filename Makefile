ASAR := tools/asar
ROM  := build/sbm2.sfc

.PHONY: all compare disasm coverage clean
all: compare

$(ROM): $(wildcard src/*.asm)
	@mkdir -p build
	head -c 1048576 /dev/zero > $@.tmp
	$(ASAR) --no-title-check src/main.asm $@.tmp
	mv $@.tmp $@

compare: $(ROM)
	@cmp $(ROM) baserom.sfc && echo "OK: $(ROM) matches baserom.sfc"

# Regenerates src/ from the ROM; overwrites any hand edits.
disasm:
	python3 -I tools/sbm2/disasm.py baserom.sfc tools/sbm2/hints.txt src $(wildcard coverage/*.cdl)

clean:
	rm -rf build

# Headless attract-mode run; writes a CDL into coverage/.
FRAMES ?= 1800
coverage:
	cd tools/mesen2 && SBM2_FRAMES=$(FRAMES) SBM2_CDL_OUT=$(CURDIR)/coverage/attract_$(FRAMES)f.cdl \
		./Mesen --testrunner $(CURDIR)/baserom.sfc $(CURDIR)/bot/cdl_dump.lua

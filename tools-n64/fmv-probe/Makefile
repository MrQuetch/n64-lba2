# Built inside the n64lba-fmv-preview image (see Dockerfile.preview) by
# fmv-probe.sh — not part of the game build.
BUILD_DIR=build
LIBDRAGON_PREVIEW = 2
include $(N64_INST)/include/n64.mk

src = fmvprobe.c

all: fmvprobe.z64

fmvprobe.z64: N64_ROM_TITLE="LBA2 FMV probe"
fmvprobe.z64: $(BUILD_DIR)/fmvprobe.dfs

$(BUILD_DIR)/fmvprobe.elf: $(src:%.c=$(BUILD_DIR)/%.o)

# Always repacked: the clip set in filesystem/ is what the probe measures.
.PHONY: $(BUILD_DIR)/fmvprobe.dfs
$(BUILD_DIR)/fmvprobe.dfs:
	@echo "    [DFS] $@"
	$(N64_MKDFS) "$@" filesystem >/dev/null

clean:
	rm -rf $(BUILD_DIR) filesystem fmvprobe.z64

-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all clean

CARGO_PRESENT := false
RUSTUP_PRESENT := false

CARGO_VERSION := $(word 2, $(shell cargo version 2>/dev/null))
CARGO_BUILD_FLAGS := --release --manifest-path Rust/generic_hash/Cargo.toml
RUSTUP_FLAGS :=
ifneq ($(filter 1.%,$(CARGO_VERSION)),)
	CARGO_PRESENT := true
ifeq (s,$(findstring s,$(MAKEFLAGS)))
    CARGO_BUILD_FLAGS += --quiet
    RUSTUP_FLAGS += --quiet
endif
endif

RUSTUP_VERSION := $(word 2, $(shell rustup --version 2>/dev/null))
ifneq ($(filter 1.%,$(RUSTUP_VERSION)),)
	RUSTUP_PRESENT := true
endif

RUST_TARGET_DIR := Rust/generic_hash/target
GENERIC_HASH_SO := $(RUST_TARGET_DIR)/release/libgeneric_hash.so
GENERIC_HASH_DLL := $(RUST_TARGET_DIR)/x86_64-pc-windows-gnu/release/generic_hash.dll
ifeq ($(BRIDGE_SUFFIX),so)
GENERIC_HASH_DEFAULT := $(GENERIC_HASH_SO)
else
GENERIC_HASH_DEFAULT := $(GENERIC_HASH_DLL)
endif

RED = \033[1;31m
RESET = \033[0m

ifeq ($(CARGO_PRESENT),true)
$(GENERIC_HASH_SO):
	cargo build $(CARGO_BUILD_FLAGS) || true
	@- mv $(RUST_TARGET_DIR)/release/libgeneric_hash.dylib $(GENERIC_HASH_SO) 2>/dev/null
ifeq ($(RUSTUP_PRESENT),true)
$(GENERIC_HASH_DLL):
	rustup $(RUSTUP_FLAGS) target add x86_64-pc-windows-gnu && \
	cargo build $(CARGO_BUILD_FLAGS) --target x86_64-pc-windows-gnu || true
else
$(GENERIC_HASH_DLL):
	@echo ""
	@echo -e "$(RED)WARNING$(RESET): Skipping regular plugin 74000: rustup not found."
	@echo "         To use -m 74000, you must install Rust (with cargo and rustup)."
	@echo "         Otherwise, you can safely ignore this warning."
	@echo "         For more information, see 'docs/hashcat-rust-plugin-requirements.md'."
	@echo ""
endif
else
$(GENERIC_HASH_SO):
	@echo ""
	@echo -e "$(RED)WARNING$(RESET): Skipping regular plugin 74000: Rust not found."
	@echo "         To use -m 74000, you must install Rust."
	@echo "         Otherwise, you can safely ignore this warning."
	@echo "         For more information, see 'docs/hashcat-rust-plugin-requirements.md'."
	@echo ""
$(GENERIC_HASH_DLL):
	@echo ""
	@echo -e "$(RED)WARNING$(RESET): Skipping regular plugin 74000: Rust not found."
	@echo "         To use -m 74000, you must install Rust (with cargo and rustup)."
	@echo "         Otherwise, you can safely ignore this warning."
	@echo "         For more information, see 'docs/hashcat-rust-plugin-requirements.md'."
	@echo ""
endif

COMMON_PREREQS := src/bridges/bridge_rust_generic_hash.c src/cpu_features.c

ifeq ($(BUILD_MODE),cross)
bridges/bridge_rust_generic_hash.so: $(COMMON_PREREQS) obj/combined.LINUX.a $(GENERIC_HASH_SO)
	$(CC_LINUX)  $(CCFLAGS) $(CFLAGS_CROSS_LINUX) $(filter-out $(GENERIC_HASH_SO) $(GENERIC_HASH_DLL),$^) -o $@ $(LFLAGS_CROSS_LINUX) -shared -fPIC -D BRIDGE_INTERFACE_VERSION_CURRENT=$(BRIDGE_INTERFACE_VERSION) $(PYTHON_CFLAGS)
bridges/bridge_rust_generic_hash.dll: $(COMMON_PREREQS) obj/combined.WIN.a $(GENERIC_HASH_DLL)
	$(CC_WIN)    $(CCFLAGS) $(CFLAGS_CROSS_WIN)   $(filter-out $(GENERIC_HASH_SO) $(GENERIC_HASH_DLL),$^) -o $@ $(LFLAGS_CROSS_WIN)   -shared -fPIC -D BRIDGE_INTERFACE_VERSION_CURRENT=$(BRIDGE_INTERFACE_VERSION) $(PYTHON_CFLAGS_WIN)
else
ifeq ($(SHARED),1)
bridges/bridge_rust_generic_hash.$(BRIDGE_SUFFIX): $(COMMON_PREREQS) $(HASHCAT_LIBRARY) $(GENERIC_HASH_DEFAULT)
	$(CC) $(CCFLAGS) $(CFLAGS_NATIVE)             $(filter-out $(GENERIC_HASH_SO) $(GENERIC_HASH_DLL),$^) -o $@ $(LFLAGS_NATIVE)      -shared -fPIC -D BRIDGE_INTERFACE_VERSION_CURRENT=$(BRIDGE_INTERFACE_VERSION) $(PYTHON_CFLAGS)
else
bridges/bridge_rust_generic_hash.$(BRIDGE_SUFFIX): $(COMMON_PREREQS) obj/combined.NATIVE.a $(GENERIC_HASH_DEFAULT)
	$(CC) $(CCFLAGS) $(CFLAGS_NATIVE)             $(filter-out $(GENERIC_HASH_SO) $(GENERIC_HASH_DLL),$^) -o $@ $(LFLAGS_NATIVE)      -shared -fPIC -D BRIDGE_INTERFACE_VERSION_CURRENT=$(BRIDGE_INTERFACE_VERSION) $(PYTHON_CFLAGS)
endif
endif
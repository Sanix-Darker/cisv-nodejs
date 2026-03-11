.PHONY: update-core core nodejs all clean
all: core nodejs
update-core:
	git submodule update --init --remote --recursive core
core:
	$(MAKE) -C core/core all
nodejs: core
	cd cisv && npm ci && npm run build
clean:
	$(MAKE) -C core/core clean
	cd cisv && rm -rf build node_modules || true

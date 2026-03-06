.PHONY: core nodejs all clean
all: core nodejs
core:
	$(MAKE) -C core/core all
nodejs: core
	cd bindings/nodejs && npm ci && npm run build
clean:
	$(MAKE) -C core/core clean
	cd bindings/nodejs && rm -rf build node_modules || true

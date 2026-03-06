.PHONY: core nodejs all clean
all: core nodejs
core:
	$(MAKE) -C core/core all
nodejs: core
	cd cisv && npm ci && npm run build
clean:
	$(MAKE) -C core/core clean
	cd cisv && rm -rf build node_modules || true

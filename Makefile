.PHONY: all module frontend tools clean
all: module frontend tools

module:
	$(MAKE) -C kernel

frontend:
	$(MAKE) -C frontend

tools:
	$(MAKE) -C tools

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C frontend clean
	$(MAKE) -C tools clean

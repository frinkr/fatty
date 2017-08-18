.PHONY: install

exe:
	cd src; $(MAKE)

install:
	cp src/fatty.exe /bin

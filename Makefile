.PHONY: all
all: syncsh

syncsh: syncsh.c
	gcc -o $@ -W -Wall -g $<

.PHONY: test
test: syncsh
	@MAKEFILE_LIST= $(MAKE) SHELL=$(CURDIR)/syncsh --no-print-directory -j par

.PHONY: par A B C D
par: A B C D
A B C D:
	@for n in 1 2 3; do echo $@$$n; sleep 1 ; done

.PHONY: install
install: all
	mv syncsh ${HOME}/${CPU}/bin

.PHONY: clean
clean:
	rm -f syncsh *~ OUT

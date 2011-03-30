.PHONY: all
all: syncsh

syncsh: syncsh.c
	gcc -o $@ -W -Wall -g $<

major	:= A B C D E
minor	:= 1 2 3 4

.PHONY: test test-good test-bad
test: test-bad test-good
test-bad: syncsh
	@echo "Below, letter groups will be scrambled :-("
	@$(MAKE) --no-print-directory -j$(words $(major)) par
test-good: syncsh
	@echo "Below, letter groups will stay together :-)"
	@MAKEFILE_LIST= $(MAKE) SHELL=$(CURDIR)/syncsh --no-print-directory -j$(words $(major)) par

.PHONY: par $(major)
par: $(major)
$(major):
	@for n in $(minor); do echo $@$$n; sleep 1 ; done

.PHONY: install
install: all
	mv syncsh ${HOME}/${CPU}/bin

.PHONY: clean
clean:
	rm -f syncsh *~ OUT

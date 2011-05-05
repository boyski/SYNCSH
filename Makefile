.PHONY: all
all: syncsh

syncsh: syncsh.c
	gcc -o $@ -W -Wall -g $<

major	:= A B C D E
minor	:= 1 2 3 4

.PHONY: test test-normal test-sync test-serial
test: test-normal test-sync test-serial
test-normal: syncsh
	@echo "These letter groups may be 'scrambled':"
	@$(MAKE) --no-print-directory -j par
test-sync: syncsh
	@echo "These letter groups should stay together:"
	@$(MAKE) SHELL=$(CURDIR)/syncsh --no-print-directory -j par
test-serial: syncsh
	@echo "These letter groups should stay together, except that 'D' runs serially:"
	SYNCSH_SERIALIZE="echo D" $(MAKE) SHELL=$(CURDIR)/syncsh --no-print-directory -j par

.PHONY: par $(major)
par: $(major)
$(major):
	@for n in $(minor); do echo $@$$n; sleep 1 ; done

.PHONY: clean
clean:
	$(RM) -f syncsh *.exe *~ OUT

installed	:= $(shell bash -c "type -p syncsh")
.PHONY: install
install: all test-sync
ifeq (,$(installed))
	@echo "First install must be manual!"; exit 1
else
	mv syncsh $(installed)
endif

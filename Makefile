# msgbox Makefile
#
# The primary rules are:
#
# * all      -- Builds everything in the out/ directory.
# * test     -- Builds and runs all tests, printing out the results.
# * examples -- Builds the examples in the out/ directory.
# * clean    -- Deletes everything this makefile may have created.
#

# For now, there are no examples (add them).

#################################################################################
# Variables for targets.

# Target lists.
tests            = out/msgbox_test out/udp_timeout_test out/multiget_test
cstructs_obj     = array.o map.o list.o memprofile.o
cstructs_rel_obj = $(addprefix out/,       $(cstructs_obj))
cstructs_dbg_obj = $(addprefix out/debug_, $(cstructs_obj))
release_obj      = out/msgbox.o $(cstructs_rel_obj)
debug_obj        = out/debug_msgbox.o $(cstructs_dbg_obj)
test_obj         = out/ctest.o $(debug_obj)
examples         = $(addprefix out/,echo_client echo_server)

# Variables for build settings.
includes = -Imsgbox -I.
ifeq ($(shell uname -s), Darwin)
	cflags = $(includes) -std=c99
else
	cflags = $(includes) -std=c99 -D _BSD_SOURCE -D _POSIX_C_SOURCE=200809
endif
cc = gcc $(cflags)

# Test-running environment.
testenv = DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_LOG_FILE=/dev/null

#################################################################################
# Primary rules; meant to be used directly.

# Build everything.
all: out/libmsgbox.a $(release_obj) $(tests) $(examples)

# Build all tests.
test: $(tests)
	@echo Running tests:
	@echo -
	@for test in $(tests); do $(testenv) $$test || exit 1; done
	@echo -
	@echo All tests passed!

# Build the examples.
examples: $(examples)

clean:
	rm -rf out

#################################################################################
# Internal rules; meant to only be used indirectly by the above rules.

out:
	mkdir -p out

out/ctest.o: test/ctest.c test/ctest.h | out
	$(cc) -o out/ctest.o -g -c test/ctest.c

out/libmsgbox.a: $(release_obj)
	ar cr $@ $^

$(cstructs_rel_obj) : out/%.o : cstructs/%.c cstructs/%.h | out
	$(cc) -o $@ -c $<

out/msgbox.o : msgbox/msgbox.c msgbox/msgbox.h | out
	$(cc) -o $@ -c $<

$(cstructs_dbg_obj) : out/debug_%.o : cstructs/%.c cstructs/%.h | out
	$(cc) -o $@ -c $< -g -DDEBUG

out/debug_msgbox.o : msgbox/msgbox.c msgbox/msgbox.h | out
	$(cc) -o $@ -c $< -g -DDEBUG

$(tests) : out/% : test/%.c $(test_obj)
	$(cc) -o $@ -g $^ -lm

$(examples) : out/% : examples/%.c out/libmsgbox.a
	$(cc) -o $@ $^

# Listing this special-name rule prevents the deletion of intermediate files.
.SECONDARY:

# The PHONY rule tells the makefile to ignore directories with the same name as a rule.
.PHONY : examples test

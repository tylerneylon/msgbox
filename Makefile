# msgbox Makefile
#
# The primary rules are:
#
# * all -- Builds everything in the out/ directory.
# * test -- Builds and runs all tests, printing out the results.
# * examples -- Builds the examples in the out/ directory.
# * clean -- Deletes everything this makefile may have created.
#

# TODO Make sure the above comments are correct.
# For now, there are no examples (add them).

#################################################################################
# Variables for targets.

# Target lists.
tests = out/msgbox_test
release_obj = $(addprefix out/,msgbox.o CArray.o CList.o CMap.o)
debug_obj= $(addprefix out/debug_,msgbox.o CArray.o CList.o CMap.o)
test_obj = out/ctest.o out/memprofile.o $(debug_obj)
examples = $(addprefix out/,udp_echo_server udp_echo_client)

# Variables for build settings.
includes = -Isrc
cflags = $(includes)
cc = clang $(cflags)

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

out/debug_%.o : src/%.c src/%.h | out
	$(cc) -o $@ -g -c $< -DDEBUG

out/%.o : src/%.c src/%.h | out
	$(cc) -o $@ -c $<

$(tests) : out/% : test/%.c $(test_obj)
	$(cc) -o $@ -g $^

$(examples) : out/% : examples/%.c out/libmsgbox.a
	$(cc) -o $@ $^

# Listing this special-name rule prevents the deletion of intermediate files.
.SECONDARY:

# The PHONY rule tells the makefile to ignore directories with the same name as a rule.
.PHONY : examples test

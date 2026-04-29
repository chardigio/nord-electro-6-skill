CC = clang
CFLAGS = -Isource -Wno-deprecated-declarations
FRAMEWORKS = -framework IOKit -framework CoreFoundation

.PHONY: all clean test test-integration

all: nord

nord: source/nord_usb.c source/nord_cli.c source/nord_usb.h source/nord_names.h source/nord_addr.h source/nord_planner.h
	$(CC) $(CFLAGS) $(FRAMEWORKS) -o $@ source/nord_usb.c source/nord_cli.c

tests/test_name_uniqueness: tests/test_name_uniqueness.c source/nord_names.h
	$(CC) $(CFLAGS) -o $@ tests/test_name_uniqueness.c

tests/test_bank_spec: tests/test_bank_spec.c source/nord_addr.h
	$(CC) $(CFLAGS) -o $@ tests/test_bank_spec.c

tests/test_planner: tests/test_planner.c source/nord_planner.h source/nord_addr.h
	$(CC) $(CFLAGS) -o $@ tests/test_planner.c

test: tests/test_name_uniqueness tests/test_bank_spec tests/test_planner
	./tests/test_name_uniqueness
	./tests/test_bank_spec
	./tests/test_planner

test-integration: nord
	bash tests/integration/verify-nord.sh

clean:
	rm -f nord tests/test_name_uniqueness tests/test_bank_spec tests/test_planner

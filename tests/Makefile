# Host unit tests: plain g++ on Linux, no emulator required.   make -C tests/host run
ROOT := ../..
USER_DIR := $(ROOT)/build/hh0_smp0_apic1/user
CXX ?= g++
SRCS := core_test.cpp $(ROOT)/src/arch/x86_64/apic.cpp $(ROOT)/src/memory/virtual/aspace.cpp $(ROOT)/src/process/elf.cpp

core_test: $(SRCS) $(wildcard $(ROOT)/src/*/*/*.hpp) $(wildcard $(ROOT)/src/*/*.hpp)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -DAOS_HOST_TEST $(SRCS) -o $@

run: core_test
	$(MAKE) -C $(ROOT) user
	./core_test $(USER_DIR)

clean:
	rm -f core_test

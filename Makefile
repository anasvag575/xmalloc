#Base flags
CC = gcc
DFLAGS = -g
WFLAGS = -Wall
OFLAGS = -O3
CFLAGS = $(DFLAGS) $(WFLAGS) $(OFLAGS)

#Flags for generation of dynamic library
CCX = g++
CXX_FLAGS_EXTRA = -shared -fPIC
CXX_FLAGS = $(CFLAGS) $(CXX_FLAGS_EXTRA)

#Linker Flags
LFLAGS = -lpthread -lm
LOCAL_LIB = -L. -lxmalloc

#Base files for allocator
SRC_BASE = allocator.cpp
SRC_INCLUDES =  allocator_header.h  	\
		allocator_internal.h  	\
		allocator.h		\
		allocator_list.h	\
		atomic.h
				
#Programm used for testing
SRC_TEST = test.c
SRC_OVERLOAD = test_overloads.cpp

#Targets
TEST_PROGRAM = test_alloc
TEST_OVERLOAD = test_overloads
LIB = libxmalloc.so

######################## Constructors ##########################

#Create dynamic library and testcases
all: $(TEST_PROGRAM) lib

lib: $(LIB)

#Create test program 
$(TEST_PROGRAM): $(SRC_TEST) $(LIB)
	$(CC) $(CFLAGS) $< $(LOCAL_LIB) -o $@ $(LFLAGS)

#Create dynamic library object
$(LIB): $(SRC_BASE) $(SRC_INCLUDES)
	$(CCX) $(CXX_FLAGS) -o $@ $<

######################## Cleaners ###############################

#Clean Objects and Created Files
clean-all: clean clean-out
clean:
	rm -vf $(TEST_PROGRAM) $(LIB)

# Xmalloc

A C/C++ implementation of a multithreaded memory allocator. Supports all the base operations like:

- Malloc/Calloc
- Realloc
- Free

This implementation is based on this paper, as part of the ECE P124 class (Advanced OS):

- **[Scott Schneider, Christos D. Antonopoulos, and Dimitrios S. Nikolopoulos. 2006. Scalable locality-conscious multithreaded memory allocation. In Proceedings of the 5th international symposium on Memory management (ISMM '06). Association for Computing Machinery, New York, NY, USA, 84–94. https://doi.org/10.1145/1133956.1133968]**

We provide an additional PDF report with our modifications and additions to the initial implementation, in order to support 64-bit architectures.

### Testing

To execute the tests, inside the folder with the Makefile and source code:

```
#Build the library and link with the testcase
make

#Run the tests
./run_test_alloc.sh
```

The library is built as a dynamic library object (**.so**), where using the PRELOAD semantics a user can inject this allocator and override the default one, which is what the example script does.

For now the library has been tested only on multiple versions of Ubuntu - x86-64 architecture. Feel free to inform me, in case an issue is found.


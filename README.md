# ODZip Alpha
Minimal file compression. 

Archives & encryption coming soon.


## Install

### macOS / Linux (Homebrew)
```sh
brew install odpay/tap/odz
```

## Building
clone the git project and you can build using either cmake, make, gcc/clang.

### Option 1 (recommended); CMake (make):
```sh
mkdir build
cd build
cmake ..
make
```

### Option 2; Using provided makefile:
```sh
mv Makefile.bak Makefile
make
```


### Option 3; build directly with gcc/clang:
```sh
gcc -std=c17 -O2 -Wall -Wextra -o odz main.c compress.c decompress.c lz_hashchain.c odz_util.c
```


## Disclaimer
This project is in early alpha, 
- it WILL not be afraid to overwrite a file if given an output that already exists
- it currently doesn't preserve file permissions/inode data.

PRs/issues are welcome.

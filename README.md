# Sonnote: A composition software based on pure intervals

**Author:** Declan McMahon  
**License:** MIT  
**Version:** 1.0.0  

## Overview

**Sonnote** is a standalone program using Vulkan and JACK to provide a composition tool based on pure intervals.

## Compilation

cmake -S . -B build (-DCMAKE_BUILD_TYPE=RelWithDebInfo)
cmake --build build -j
./build/app

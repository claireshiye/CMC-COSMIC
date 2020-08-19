# Installation Instructions

## Create virtual environment
```
conda install -c conda-forge openmpi-mpicc clang_osx-64 clangxx_osx-64 gsl cfitsio gfortran_linux-64 autoconf libtool automake pkg-config binutils
```

## install CMC
```
mkdir build
cd build
CONDA_BUILD_SYSROOT=/ FC=mpifort CC=mpicc cmake .. -DCMAKE_INSTALL_PREFIX=.
CONDA_BUILD_SYSROOT=/ make install
```
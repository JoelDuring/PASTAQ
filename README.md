# Installation

To install from source, you will need to install a suitable C++ compiler and
corresponding build tools for your platform as well as CMake and zlib. Note that
on Windows you must use `zlibstatic` to avoid DLL path errors with the bindings.
The instructions listed here refer to the installation of PASTAQ's Python
bindings.  Currently the only external dependencies other than zlib are included
as git submodules.  To get started, clone this repository and initialize git
submodules:

```
git clone https://github.com/PASTAQ-MS/PASTAQ.git
cd PASTAQ
git submodule init
git submodule update
```

Build the module and install it in your system:

```sh
# Installation
python3 setup.py install --user

# Development
python3 setup.py develop --user
```

Now it can be imported and used in python as follows:

```python
import pastaq
raw_data = pastaq.read_mzxml(...)
```

# Usage

Examples of the usage of the PASTAQ can be found in the `examples` folder. To
run them, install pastaq as previously described, update the input path of the
mzXML and mzID files, change any necessary parameters and run it with:

```
python examples/small_range.py
```

You can use any mzXML files and identifications in mzIdentML v1.1+. If no
identifications are available, remove the `ident_path` from the input files
array or set it to `'none'`. You can find the files we used for testing and
development via ProteomeXchange, with identifier PXD024584.

Processing of mzML files is in an early stage and may lead to some issues.

For more information about PASTAQ and the configuration of the parameters,
please visit [the official website][website].

[website]: https://pastaq.horvatovichlab.com

# Compile the pastaq library from source

For an out of source build of the library, create a build directory and run cmake:

```sh
mkdir build
cd build
cmake ..
make
```

If you wish to enable the compilation of the tests you need to set up the
`PASTAQ_ENABLE_TESTS` flag to 1. A limited number of automated test are
currently available but we are looking forward to improve this in the future.

```sh
mkdir build
cd build
cmake .. -DPASTAQ_ENABLE_TESTS=1
make
make test
```

Additionally, you can use the Ninja building tool for faster compilation times.

```sh
mkdir build
cd build
cmake .. -DPASTAQ_ENABLE_TESTS=1 -GNinja
ninja
ninja test
```

# How to cite this work

The main manuscript associated with this software has been submitted and is
under peer review at the moment. You can find a pre-print to the current version
here on [Research Square](https://www.researchsquare.com/article/rs-422254/v1)
with DOI: [10.21203/rs.3.rs-422254/v1](https://doi.org/10.21203/rs.3.rs-422254/v1).

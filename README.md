

# Develop using Eclipse 

Assume the source file is downloaded in a folder called `parmcb`. Create a 
folder called `parmcb-build` parallel to the `parmcb` folder. This works best 
when using Eclipse. 

Switch to your new folder `parmcb-build` and issue 

```
cmake ../parmcb/ -G"Eclipse CDT4 - Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
```

in order to build using debugging symbols. 

Open up Eclipse and import an existing project into the workspace.


# Compile using Clang

Use 

```
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ../parmcb/ -G"Eclipse CDT4 - Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
```

# Requirements

The library requires boost and TBB (Intel Threading Building Blocks).

See the following [guide](https://software.intel.com/en-us/articles/installing-intel-free-libs-and-python-apt-repo) 
on how to install TBB in Ubuntu based systems.

If cmake fails to locate TBB, try something like 

```
TBBROOT=/apps/compilers/intel/19.0.1/tbb cmake ../parmcb/
```

# Linker errors

If you are experiencing any linker errors with the boost libraries, then you boost libraries
might have been compiled with an older ABI. In that case a possible workaround is to use 

```
add_definitions(-D_GLIBCXX_USE_CXX11_ABI=0)
```

in the `CMakeLists.txt` file.


Happy coding!

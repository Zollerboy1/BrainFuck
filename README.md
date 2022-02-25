# BrainFuck2Executable compiler

This is a little BrainFuck compiler that produces native executables using LLVM as the backend.

## Building

After you have cloned this repository (recursively so that the submodules get cloned as well),
you can just build the compiler by invoking the `build_script.py` file.
This should then build the LLVM libraries and afterwards the compiler itself.
For this to work, you will need to have cmake installed on your system.

## Usage

After you have built the compiler, you can run it like so:

```bash
./build/bin/BrainFuck path/to/brainfuck/file.bf
```

This will then compile the BrainFuck code to an executable which you can find in the folder
of the file you gave as the input.

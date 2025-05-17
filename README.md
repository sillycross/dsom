## DSOM -- A standard-compliant SOM VM powered by Deegen

### Building 

To build the project, make sure you have `docker` and `python3` installed, and run:
```
python3 dsom-build make release
```

Once the build is complete, you should see an executable `dsom` in the repository root directory. You can use it to run your SOM script, for example:
```
./dsom -cp ./Smalltalk ./Hello.som
```
will output
```
Hello World!
```

<!--### Note

SOM specification did not specify the minimum bit-width of integers. Unlike most SOM implementations (which uses 64 or 63-bit integer), we use 32-bit integer for simplicity. Supporting 64-bit integer is completely possible, but only "uninteresting" engineering work from a research perspective. This does not affect any of the benchmarks, but unfortunately results in a few failed tests in SOM's standard test suite, which assumes 64 or 63-bit integers. -->

### License

[Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0).


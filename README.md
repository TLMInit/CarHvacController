| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- |

# _Sample project_

(See the README.md file in the upper level 'examples' directory for more information about examples.)

This is the simplest buildable example. The example is used by command `idf.py create-project`
that copies the project to user specified path and set it's name. For more information follow the [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project)



## How to use example
We encourage the users to use the example as a template for the new projects.
A recommended way is to follow the instructions on a [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project).

## Example folder contents

The project **sample_project** contains one source file in C language [main.c](main/main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt`
files that provide set of directives and instructions describing the project's source files and targets
(executable, library, or both). 

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── main
│   ├── CMakeLists.txt
│   └── main.c
└── README.md                  This is the file you are currently reading
```
Additionally, the sample project contains Makefile and component.mk files, used for the legacy Make based build system. 
They are not used or needed when building with CMake and idf.py.

## Tests

Host unit tests (no ESP-IDF, no hardware) live in `test/host/`: settings table, NVS schema/migration
(`settings_ns_open` against a fake NVS), dew point and defog, cascade control, COBS + CRC and the pad rule.
Firmware sources are compiled as they are; ESP-IDF headers are faked under `test/host/fakes/`.

```
cmake -S test/host -B ../tbuild-display
cmake --build ../tbuild-display
ctest --test-dir ../tbuild-display --output-on-failure
```

Add `-DRUN_KNOWN_BUG_TESTS=ON` to also build checks that document known firmware bugs (they fail).
CI (`.github/workflows/ci.yml`) runs these tests, then builds with `espressif/idf:v6.1` and fails on any
compiler warning from `main/`.
